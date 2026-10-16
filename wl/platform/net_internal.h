// SPDX-License-Identifier: MIT
// Shared by net.c and the two backends (wii/net_wii.c, host/net_host.c): not for applications.
#pragma once

#include "wl/platform/net.h"

// A system errno value (positive, newlib's or the host's) as a result. Remembers it for
// wl_net_last_errno. Never returns WL_OK.
WlResult wl_net_result_from_errno(int error);

// The protocol of each open socket (net.c): set when a backend opens or accepts one, cleared when
// it closes one. Sockets numbered beyond the table are not tracked.
void wl_net_proto_set(int32_t socket, WlNetProto proto);
void wl_net_proto_clear(int32_t socket);

// The log sinks' tee to the remote log (net_stream.h): the flag is read on every line, the call is
// made only when it is set. `text` is the sink's formatted line, without a newline.
#include <stdatomic.h>
extern atomic_bool g_wl_net_stream_on;
void wl_net_stream_log_line(const char *text, size_t len);
static inline void wl_net_stream_tee(const char *text, size_t len) {
    if (atomic_load_explicit(&g_wl_net_stream_on, memory_order_relaxed)) {
        wl_net_stream_log_line(text, len);
    }
}
