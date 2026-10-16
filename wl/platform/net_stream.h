// SPDX-License-Identifier: MIT
// A run's remote log and result files: every log line, and the files the run writes, go over one
// TCP connection to a program on a PC (tools/wii_net_run.py), so that a program on a console with
// no cable to a computer is run, watched and collected without moving the SD card.
//
// It costs nothing while it is off (the log sinks test one flag), and it never stalls the program
// when it is on: the log lines go in a ring that a thread of its own empties into a non-blocking
// socket; a ring that is full drops the new line and counts it (the next line that fits says how
// many were lost); a peer that stops reading for WL_NET_STREAM_STALL_MS is let go, and the thread
// connects again. All the network work, the waiting for DHCP and the connecting included, is that
// thread's.
//
// Which end connects. The spec given to wl_net_stream_open is "<host>:<port>" (the program connects
// to a listener on the PC) or "listen:<port>" (the program listens and the PC connects, which is
// the way through a NAT that hides the PC: WSL2's, for one).
//
// The protocol is text lines, then raw bytes where a line says so:
//   HELLO WLNET1 <run name>\n   first on each connection
//   <log line>\n                the sink's formatted line ("[I][tag] message"); a line that starts
//                               with a word below has a space put before it
//   FILE <name> <size>\n<size bytes>   a file written by the run; <name> is relative, no spaces
//   END\n                       the last thing sent, when the program leaves
// and from the PC to the program: "QUIT\n" leaves for the loader (the program's wl_platform_exit
// with WL_EXIT_LOADER, or what wl_net_stream_set_quit returned), which is how a hung run is ended
// and the Homebrew Channel got back for the next wiiload.
#pragma once

#include "wl/base/base.h"

#define WL_NET_STREAM_STALL_MS 3000u  // a send that moves nothing for this long drops the connection
#define WL_NET_STREAM_FILE_MAX (16u << 20) // larger files are not sent

typedef struct {
    bool connected;
    uint32_t connects;       // connections made (the first included)
    uint32_t lines_sent;     // log lines handed to the socket
    uint32_t lines_dropped;  // lines that found the ring full
    uint32_t lines_lost;     // lines in flight when a connection broke
    uint32_t bytes_sent;
    uint32_t files_sent;
    uint32_t files_dropped;  // queue full, too large, unreadable, or the connection broke in it
} WlNetStreamStats;

// Starts it: `spec` as above ("192.168.0.5:7777", a host name works too, or "listen:7777"),
// `run_name` for the HELLO line (letters, digits and "._-" only; others become "_"). Returns at
// once; the thread brings the network up (wl_net_start), connects and sends what the ring holds.
// Installs the sinks' tee and a shutdown hook that flushes (wl_net_stream_close). WL_ERR_FORMAT for
// a bad spec, WL_ERR_STATE if already open, WL_ERR_NOMEM.
WlResult wl_net_stream_open(const char *spec, const char *run_name);
bool wl_net_stream_active(void);

// Sends the files the run writes, once each is closed (platform.h's written hook): those directly
// in `root` ("sd:/xbox/halo") and in its subfolders named in `dirs` (comma separated, "play,x";
// NULL for none). The name sent is the path below `root`. Needs wl_net_stream_open.
WlResult wl_net_stream_watch_files(const char *root, const char *dirs);
// Queues one file (read by the thread when its turn comes; leave it as it is until then).
// WL_ERR_LIMIT when the queue is full, WL_ERR_STATE when the stream is not open.
WlResult wl_net_stream_send_file(const char *path, const char *name);

void wl_net_stream_stats(WlNetStreamStats *out);

// What QUIT does instead of leaving for the loader (tests); NULL restores it.
void wl_net_stream_set_quit(void (*fn)(void));

// Sends what is queued, then END, waiting at most `flush_ms` for the PC, and closes. The shutdown
// hook calls it with 3000. Safe to call when not open, twice, and from the stream's own thread.
void wl_net_stream_close(uint32_t flush_ms);
