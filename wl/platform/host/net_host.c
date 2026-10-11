// SPDX-License-Identifier: MIT
// net.h on the host: POSIX sockets. The network is always up, so the tests of code written against
// net.h run on loopback; the results are the console's (net_internal.h maps errno alike).
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <string.h>
#include <unistd.h>

#include "wl/platform/net_internal.h"

static int fail(void) {
    return wl_net_result_from_errno(errno);
}

static void to_system(const WlNetAddr *address, struct sockaddr_in *out) {
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    out->sin_port = htons(address->port);
    out->sin_addr.s_addr = htonl(address->address);
}

static void from_system(const struct sockaddr_in *in, WlNetAddr *out) {
    out->address = ntohl(in->sin_addr.s_addr);
    out->port = ntohs(in->sin_port);
}

WlResult wl_net_start(void) {
    return WL_OK;
}

WlNetState wl_net_state(void) {
    return WL_NET_UP;
}

WlResult wl_net_wait(uint32_t timeout_ms) {
    (void)timeout_ms;
    return WL_OK;
}

void wl_net_stop(void) {}

uint32_t wl_net_local_address(void) {
    return WL_NET_ADDR(127, 0, 0, 1);
}

WlResult wl_net_resolve(const char *host, uint32_t *out) {
#if !defined(__powerpc__)
    struct addrinfo hints, *found = NULL;
#endif

    if (host == NULL || out == NULL) {
        return WL_ERR_ARG;
    }
    if (wl_net_parse_address(host, out) == WL_OK) {
        return WL_OK;
    }
#if defined(__powerpc__)
    // (the ppc-be test build links statically, where glibc's resolver wants shared libraries at run
    // time: only dotted addresses there)
    return WL_ERR_UNSUPPORTED;
#else
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &found) != 0 || found == NULL) {
        return WL_ERR_NOT_FOUND;
    }
    *out = ntohl(((const struct sockaddr_in *)(const void *)found->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(found);
    return WL_OK;
#endif
}

WlResult wl_net_socket(WlNetProto proto, WlSocket *out) {
    int fd;

    if (out == NULL || (proto != WL_NET_UDP && proto != WL_NET_TCP)) {
        return WL_ERR_ARG;
    }
    fd = socket(AF_INET, proto == WL_NET_TCP ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (fd < 0) {
        return fail();
    }
    wl_net_proto_set(fd, proto);
    *out = fd;
    return WL_OK;
}

WlResult wl_net_close(WlSocket socket) {
    wl_net_proto_clear(socket);
    return close(socket) == 0 ? WL_OK : fail();
}

WlResult wl_net_bind(WlSocket socket, const WlNetAddr *address) {
    struct sockaddr_in system;

    if (address == NULL) {
        return WL_ERR_ARG;
    }
    to_system(address, &system);
    return bind(socket, (const struct sockaddr *)&system, sizeof system) == 0 ? WL_OK : fail();
}

WlResult wl_net_local_addr(WlSocket socket, WlNetAddr *out) {
    struct sockaddr_in system;
    socklen_t length = sizeof system;

    if (out == NULL) {
        return WL_ERR_ARG;
    }
    if (getsockname(socket, (struct sockaddr *)&system, &length) != 0) {
        return fail();
    }
    from_system(&system, out);
    return WL_OK;
}

WlResult wl_net_connect(WlSocket socket, const WlNetAddr *address) {
    struct sockaddr_in system;

    if (address == NULL) {
        return WL_ERR_ARG;
    }
    to_system(address, &system);
    return connect(socket, (const struct sockaddr *)&system, sizeof system) == 0 ? WL_OK : fail();
}

WlResult wl_net_listen(WlSocket socket, int32_t backlog) {
    return listen(socket, backlog) == 0 ? WL_OK : fail();
}

WlResult wl_net_accept(WlSocket socket, WlSocket *out, WlNetAddr *peer) {
    struct sockaddr_in system;
    socklen_t length = sizeof system;
    int fd;

    if (out == NULL) {
        return WL_ERR_ARG;
    }
    fd = accept(socket, (struct sockaddr *)&system, &length);
    if (fd < 0) {
        return fail();
    }
    if (peer != NULL) {
        from_system(&system, peer);
    }
    wl_net_proto_set(fd, WL_NET_TCP);
    *out = fd;
    return WL_OK;
}

int32_t wl_net_send(WlSocket socket, const void *data, uint32_t size) {
    ssize_t sent = send(socket, data, size, MSG_NOSIGNAL);

    return sent < 0 ? fail() : (int32_t)sent;
}

int32_t wl_net_recv(WlSocket socket, void *buffer, uint32_t size) {
    ssize_t received = recv(socket, buffer, size, 0);

    return received < 0 ? fail() : (int32_t)received;
}

int32_t wl_net_sendto(WlSocket socket, const void *data, uint32_t size, const WlNetAddr *to) {
    struct sockaddr_in system;
    ssize_t sent;

    if (to == NULL) {
        return WL_ERR_ARG;
    }
    to_system(to, &system);
    sent = sendto(socket, data, size, MSG_NOSIGNAL, (const struct sockaddr *)&system, sizeof system);
    return sent < 0 ? fail() : (int32_t)sent;
}

int32_t wl_net_recvfrom(WlSocket socket, void *buffer, uint32_t size, WlNetAddr *from) {
    struct sockaddr_in system;
    socklen_t length = sizeof system;
    ssize_t received = recvfrom(socket, buffer, size, 0, (struct sockaddr *)&system, &length);

    if (received < 0) {
        return fail();
    }
    if (from != NULL) {
        memset(from, 0, sizeof *from);
        if (length >= sizeof system) {
            from_system(&system, from);
        }
    }
    return (int32_t)received;
}

WlResult wl_net_shutdown(WlSocket socket, WlNetShutdown how) {
    static const int systems[3] = {SHUT_RD, SHUT_WR, SHUT_RDWR};

    if ((unsigned)how > (unsigned)WL_NET_SHUT_BOTH) {
        return WL_ERR_ARG;
    }
    return shutdown(socket, systems[how]) == 0 ? WL_OK : fail();
}

WlResult wl_net_set_nonblocking(WlSocket socket, bool nonblocking) {
    int flags = fcntl(socket, F_GETFL, 0);

    if (flags < 0) {
        return fail();
    }
    flags = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(socket, F_SETFL, flags) == 0 ? WL_OK : fail();
}

WlResult wl_net_bytes_available(WlSocket socket, uint32_t *out) {
    int count = 0;

    if (out == NULL) {
        return WL_ERR_ARG;
    }
    if (ioctl(socket, FIONREAD, &count) != 0) {
        return fail();
    }
    *out = count < 0 ? 0u : (uint32_t)count;
    return WL_OK;
}

WlResult wl_net_set_option(WlSocket socket, WlNetOption option, int32_t value) {
    int level = SOL_SOCKET, name;
    int on = value;

    switch (option) {
    case WL_NET_OPT_NODELAY: level = IPPROTO_TCP; name = TCP_NODELAY; on = value != 0; break;
    case WL_NET_OPT_BROADCAST: name = SO_BROADCAST; on = value != 0; break;
    case WL_NET_OPT_REUSEADDR: name = SO_REUSEADDR; on = value != 0; break;
    case WL_NET_OPT_SNDBUF: name = SO_SNDBUF; break;
    case WL_NET_OPT_RCVBUF: name = SO_RCVBUF; break;
    default: return WL_ERR_ARG;
    }
    return setsockopt(socket, level, name, &on, sizeof on) == 0 ? WL_OK : fail();
}

WlResult wl_net_connect_result(WlSocket socket, WlResult *out) {
    int error = 0;
    socklen_t length = sizeof error;

    if (out == NULL) {
        return WL_ERR_ARG;
    }
    if (getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &length) != 0) {
        return fail();
    }
    *out = error == 0 ? WL_OK : wl_net_result_from_errno(error);
    return WL_OK;
}

int32_t wl_net_poll(WlNetPoll *items, uint32_t count, int32_t timeout_ms) {
    struct pollfd fds[WL_NET_POLL_MAX_SOCKETS];
    int ready, counted = 0;

    if ((items == NULL && count > 0) || count > WL_NET_POLL_MAX_SOCKETS) {
        return count > WL_NET_POLL_MAX_SOCKETS ? WL_ERR_LIMIT : WL_ERR_ARG;
    }
    for (uint32_t i = 0; i < count; i++) {
        fds[i].fd = items[i].socket;
        fds[i].events = (short)(((items[i].events & WL_NET_POLL_READ) ? POLLIN : 0) |
                                ((items[i].events & WL_NET_POLL_WRITE) ? POLLOUT : 0) |
                                ((items[i].events & WL_NET_POLL_PRIORITY) ? POLLPRI : 0));
        fds[i].revents = 0;
    }
    ready = poll(fds, count, timeout_ms < 0 ? -1 : timeout_ms);
    if (ready < 0) {
        return fail();
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t revents = 0;

        revents |= (fds[i].revents & POLLIN) ? WL_NET_POLL_READ : 0u;
        revents |= (fds[i].revents & POLLOUT) ? WL_NET_POLL_WRITE : 0u;
        revents |= (fds[i].revents & POLLPRI) ? WL_NET_POLL_PRIORITY : 0u;
        revents |= (fds[i].revents & (POLLERR | POLLNVAL)) ? WL_NET_POLL_ERROR : 0u;
        revents |= (fds[i].revents & POLLHUP) ? WL_NET_POLL_HUP : 0u;
        items[i].revents = revents;
        counted += revents != 0;
    }
    return counted;
}
