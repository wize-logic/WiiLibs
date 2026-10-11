// SPDX-License-Identifier: MIT
// The part of net.h that needs no sockets: result names, errno to result, the dotted-decimal
// address text. Both backends (wii/net_wii.c, host/net_host.c) use the errno mapping, and both
// systems' <errno.h> name the values it tests (newlib's numbers are the ones libogc returns).
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>

#include "wl/platform/net_internal.h"

static atomic_int g_last_errno;

// 0 closed, 1 UDP, 2 TCP
#define PROTO_TABLE 64
static atomic_uchar g_proto[PROTO_TABLE];

void wl_net_proto_set(int32_t socket, WlNetProto proto) {
    if (socket >= 0 && socket < PROTO_TABLE) {
        atomic_store(&g_proto[socket], (unsigned char)(proto == WL_NET_TCP ? 2 : 1));
    }
}

void wl_net_proto_clear(int32_t socket) {
    if (socket >= 0 && socket < PROTO_TABLE) {
        atomic_store(&g_proto[socket], 0);
    }
}

WlResult wl_net_socket_proto(WlSocket socket, WlNetProto *out) {
    unsigned char kind;

    if (out == NULL) {
        return WL_ERR_ARG;
    }
    kind = (socket >= 0 && socket < PROTO_TABLE) ? atomic_load(&g_proto[socket]) : 0;
    if (kind == 0) {
        return WL_ERR_NET_NOTSOCK;
    }
    *out = kind == 2 ? WL_NET_TCP : WL_NET_UDP;
    return WL_OK;
}

int wl_net_last_errno(void) {
    return atomic_load(&g_last_errno);
}

WlResult wl_net_result_from_errno(int error) {
    atomic_store(&g_last_errno, error);
    if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR) {
        return WL_ERR_AGAIN;
    }
    switch (error) {
    case ENETDOWN: return WL_ERR_NET_DOWN;
    case ENETUNREACH:
    case EHOSTUNREACH:
    case EHOSTDOWN: return WL_ERR_NET_UNREACH;
    case ECONNREFUSED: return WL_ERR_NET_REFUSED;
    case ECONNRESET:
    case ECONNABORTED:
    case ENETRESET:
    case EPIPE:
#ifdef ESHUTDOWN
    case ESHUTDOWN:
#endif
        return WL_ERR_NET_RESET;
    case ETIMEDOUT: return WL_ERR_NET_TIMEDOUT;
    case ENOTCONN: return WL_ERR_NET_NOTCONN;
    case EISCONN: return WL_ERR_NET_ISCONN;
    case EINPROGRESS: return WL_ERR_NET_INPROGRESS;
    case EALREADY: return WL_ERR_NET_ALREADY;
    case EADDRINUSE: return WL_ERR_NET_ADDRINUSE;
    case EADDRNOTAVAIL: return WL_ERR_NET_ADDRNOTAVAIL;
    case EMSGSIZE: return WL_ERR_NET_MSGSIZE;
    case EACCES:
    case EPERM: return WL_ERR_NET_ACCESS;
    case ENOTSOCK:
    case EBADF: return WL_ERR_NET_NOTSOCK;
    case EINVAL:
    case EDESTADDRREQ:
    case EAFNOSUPPORT:
    case EPROTONOSUPPORT:
    case EPROTOTYPE:
    case ENOPROTOOPT:
    case EOPNOTSUPP: return WL_ERR_ARG;
    case ENOMEM:
    case ENOBUFS:
    case EMFILE:
    case ENFILE: return WL_ERR_NOMEM;
    default: return WL_ERR_FAILED;
    }
}

const char *wl_net_result_name(WlResult result) {
    switch (result) {
    case WL_ERR_NET_DOWN: return "WL_ERR_NET_DOWN";
    case WL_ERR_NET_UNREACH: return "WL_ERR_NET_UNREACH";
    case WL_ERR_NET_REFUSED: return "WL_ERR_NET_REFUSED";
    case WL_ERR_NET_RESET: return "WL_ERR_NET_RESET";
    case WL_ERR_NET_TIMEDOUT: return "WL_ERR_NET_TIMEDOUT";
    case WL_ERR_NET_NOTCONN: return "WL_ERR_NET_NOTCONN";
    case WL_ERR_NET_ISCONN: return "WL_ERR_NET_ISCONN";
    case WL_ERR_NET_INPROGRESS: return "WL_ERR_NET_INPROGRESS";
    case WL_ERR_NET_ALREADY: return "WL_ERR_NET_ALREADY";
    case WL_ERR_NET_ADDRINUSE: return "WL_ERR_NET_ADDRINUSE";
    case WL_ERR_NET_ADDRNOTAVAIL: return "WL_ERR_NET_ADDRNOTAVAIL";
    case WL_ERR_NET_MSGSIZE: return "WL_ERR_NET_MSGSIZE";
    case WL_ERR_NET_ACCESS: return "WL_ERR_NET_ACCESS";
    case WL_ERR_NET_NOTSOCK: return "WL_ERR_NET_NOTSOCK";
    default: return wl_result_name(result);
    }
}

WlResult wl_net_parse_address(const char *text, uint32_t *out) {
    uint32_t address = 0;

    if (text == NULL || out == NULL) {
        return WL_ERR_ARG;
    }
    for (int field = 0; field < 4; field++) {
        uint32_t value = 0;
        int digits = 0;

        while (text[0] >= '0' && text[0] <= '9') {
            value = value * 10u + (uint32_t)(text[0] - '0');
            digits++;
            text++;
            if (value > 255u || digits > 3) {
                return WL_ERR_FORMAT;
            }
        }
        if (digits == 0 || (digits > 1 && text[-digits] == '0')) {
            return WL_ERR_FORMAT; // (a leading zero would be octal to inet_aton: refused)
        }
        address = (address << 8) | value;
        if (field < 3) {
            if (*text++ != '.') {
                return WL_ERR_FORMAT;
            }
        }
    }
    if (*text != '\0') {
        return WL_ERR_FORMAT;
    }
    *out = address;
    return WL_OK;
}

void wl_net_format_address(uint32_t address, char *out) {
    snprintf(out, 16, "%u.%u.%u.%u", (unsigned)(address >> 24), (unsigned)((address >> 16) & 0xffu),
             (unsigned)((address >> 8) & 0xffu), (unsigned)(address & 0xffu));
}
