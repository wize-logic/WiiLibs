// SPDX-License-Identifier: MIT
// A run's remote log and result files (net_stream.h). Portable over net.h, the thread and file API
// of platform.h: the Wii and the host run the same code, so the host's loopback test covers it.
//
// Two sides. The producers (the log sinks, on any thread; the written-file hook) only put bytes in
// a ring, or a path in a queue, under a short lock, and never wait for the network. One thread
// does everything else: brings the network up, connects (or listens), sends, reads the PC's QUIT.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/platform/mem.h"
#include "wl/platform/net_internal.h"
#include "wl/platform/net_stream.h"
#include "wl/platform/platform.h"

#define TAG "wlnet"
#define RING_BYTES 32768u // (a power of two: the counters wrap)
#define LINE_BYTES 400u
#define CHUNK_BYTES 2048u
#define FILE_CHUNK 4096u
#define FILE_QUEUE 8u
#define STACK_BYTES (24u * 1024u)
#define CONNECT_MS 3000u
#define RETRY_MS 1000u
#define DIAG_MS 5000u
#define SHUTDOWN_FLUSH_MS 3000u

typedef struct {
    char path[WL_FS_PATH_MAX];
    char name[96];
} FileJob;

atomic_bool g_wl_net_stream_on;

static struct {
    atomic_bool open;
    atomic_bool closing;   // send what is queued, then END
    atomic_bool abandon;   // stop at once
    atomic_bool done;      // the thread has sent END (or has nothing more to do)
    bool in_sender_exit;   // the thread itself is leaving (QUIT): it flushes inline
    bool quit_seen;        // a QUIT was acted on (a second one, read while flushing inline, is not)
    void (*quit_fn)(void);

    char host[64];
    uint16_t port;
    bool listen;
    char run[48];

    WlPlatformMutex lock;  // the ring, the file queue
    bool lock_made;
    char *ring;            // RING_BYTES
    uint32_t head, tail;   // counters; the index is `& (RING_BYTES - 1)`
    uint32_t pending_drop; // lines dropped since the last note
    FileJob files[FILE_QUEUE];
    uint32_t file_head, file_tail;

    char watch_root[WL_FS_PATH_MAX];
    char watch_dirs[96];

    WlPlatformThread thread;
    void *stack;
    bool stack_in_mem;
    bool joined;

    WlSocket listener;     // listen mode
    WlSocket sock;         // the connection, or WL_SOCKET_INVALID
    uint32_t lost_note;    // lines lost with the last connection, to be said on the next
    char *buffer;          // CHUNK_BYTES + FILE_CHUNK, the thread's

    atomic_uint connects, lines_sent, lines_dropped, lines_lost, bytes_sent, files_sent, files_dropped;
    atomic_bool connected;
} g = {.listener = WL_SOCKET_INVALID, .sock = WL_SOCKET_INVALID};

// ---- the producers -------------------------------------------------------------------------------

static void ring_put(const char *data, uint32_t n) {
    uint32_t at = g.head & (RING_BYTES - 1u);
    uint32_t first = RING_BYTES - at < n ? RING_BYTES - at : n;
    memcpy(g.ring + at, data, first);
    memcpy(g.ring, data + first, n - first);
    g.head += n;
}

// Puts the "lines dropped" note in the ring when `extra` more bytes fit beside it (the lock is held).
// The producer tries before each line; the sender tries too, so a note still waiting when the log goes
// quiet (the line after the drops fitted, the note beside it did not) is sent once the ring has room.
static void ring_drop_note(uint32_t extra) {
    char note[48];

    if (g.pending_drop == 0u) {
        return;
    }
    int m = snprintf(note, sizeof note, "[W][" TAG "] %u lines dropped\n", (unsigned)g.pending_drop);
    if (m > 0 && RING_BYTES - (g.head - g.tail) >= (uint32_t)m + extra) {
        ring_put(note, (uint32_t)m);
        g.pending_drop = 0;
    }
}

static bool starts_with_word(const char *text, size_t len) {
    return (len >= 5u && memcmp(text, "FILE ", 5) == 0) || (len >= 6u && memcmp(text, "HELLO ", 6) == 0) ||
           (len >= 3u && memcmp(text, "END", 3) == 0) || (len >= 4u && memcmp(text, "QUIT", 4) == 0);
}

void wl_net_stream_log_line(const char *text, size_t len) {
    char line[LINE_BYTES + 2u];
    uint32_t n = 0;

    if (!atomic_load(&g_wl_net_stream_on) || g.ring == NULL) {
        return;
    }
    if (len > LINE_BYTES) {
        len = LINE_BYTES;
    }
    if (starts_with_word(text, len)) {
        line[n++] = ' '; // (a line of the log is never taken for the protocol's)
    }
    for (size_t i = 0; i < len; i++) {
        line[n++] = text[i] == '\n' || text[i] == '\r' ? ' ' : text[i];
    }
    line[n++] = '\n';
    wl_platform_mutex_lock(g.lock);
    ring_drop_note(n);
    uint32_t space = RING_BYTES - (g.head - g.tail);
    if (space < n) {
        g.pending_drop++;
        atomic_fetch_add(&g.lines_dropped, 1u);
    } else {
        ring_put(line, n);
    }
    wl_platform_mutex_unlock(g.lock);
}

// Takes up to `cap` bytes of whole lines out of the ring; the count of lines in *lines.
static uint32_t ring_take(char *out, uint32_t cap, uint32_t *lines) {
    uint32_t n = 0;

    *lines = 0;
    wl_platform_mutex_lock(g.lock);
    ring_drop_note(0u);
    uint32_t avail = g.head - g.tail;
    if (avail > cap) {
        avail = cap;
    }
    if (avail > 0u) {
        uint32_t at = g.tail & (RING_BYTES - 1u);
        uint32_t first = RING_BYTES - at < avail ? RING_BYTES - at : avail;
        memcpy(out, g.ring + at, first);
        memcpy(out + first, g.ring, avail - first);
        n = avail;
        while (n > 0u && out[n - 1u] != '\n') {
            n--; // (end on a line: the rest goes in the next chunk)
        }
        g.tail += n;
    }
    wl_platform_mutex_unlock(g.lock);
    for (uint32_t i = 0; i < n; i++) {
        *lines += out[i] == '\n';
    }
    return n;
}

static bool ring_empty(void) {
    wl_platform_mutex_lock(g.lock);
    bool empty = g.head == g.tail;
    wl_platform_mutex_unlock(g.lock);
    return empty;
}

WlResult wl_net_stream_send_file(const char *path, const char *name) {
    if (path == NULL || name == NULL || strlen(path) >= sizeof g.files[0].path || strlen(name) >= sizeof g.files[0].name) {
        return WL_ERR_ARG;
    }
    if (!atomic_load(&g.open)) {
        return WL_ERR_STATE;
    }
    WlResult r = WL_OK;
    wl_platform_mutex_lock(g.lock);
    uint32_t used = g.file_head - g.file_tail;
    bool queued = false;
    for (uint32_t i = 0; i < used; i++) {
        queued |= strcmp(g.files[(g.file_tail + i) % FILE_QUEUE].path, path) == 0; // (a rewritten dump: once)
    }
    if (!queued) {
        if (used >= FILE_QUEUE) {
            r = WL_ERR_LIMIT;
            atomic_fetch_add(&g.files_dropped, 1u);
        } else {
            FileJob *job = &g.files[g.file_head % FILE_QUEUE];
            snprintf(job->path, sizeof job->path, "%s", path);
            snprintf(job->name, sizeof job->name, "%s", name);
            for (char *c = job->name; *c != '\0'; c++) {
                if ((unsigned char)*c <= ' ' || *c == '\\') {
                    *c = '_';
                }
            }
            g.file_head++;
        }
    }
    wl_platform_mutex_unlock(g.lock);
    return r;
}

static bool file_pop(FileJob *out) {
    bool got = false;

    wl_platform_mutex_lock(g.lock);
    if (g.file_head != g.file_tail) {
        *out = g.files[g.file_tail % FILE_QUEUE];
        g.file_tail++;
        got = true;
    }
    wl_platform_mutex_unlock(g.lock);
    return got;
}

static bool file_pending(void) {
    wl_platform_mutex_lock(g.lock);
    bool pending = g.file_head != g.file_tail;
    wl_platform_mutex_unlock(g.lock);
    return pending;
}

// The written-file hook: a file closed in the watched folder (or one of its listed subfolders).
static void on_written(void *user, const char *path) {
    char name[96];
    size_t root_len = strlen(g.watch_root);

    (void)user;
    if (strncmp(path, g.watch_root, root_len) != 0 || path[root_len] != '/') {
        return;
    }
    const char *rel = path + root_len + 1u;
    const char *slash = strchr(rel, '/');
    if (slash != NULL) {
        size_t dir_len = (size_t)(slash - rel);
        bool listed = false;
        const char *p = g.watch_dirs;
        while (*p != '\0') {
            const char *end = strchr(p, ',');
            size_t len = end != NULL ? (size_t)(end - p) : strlen(p);
            listed |= len == dir_len && memcmp(p, rel, len) == 0;
            p += len + (end != NULL ? 1u : 0u);
        }
        if (!listed || strchr(slash + 1, '/') != NULL) {
            return;
        }
    }
    if (strlen(rel) >= sizeof name) {
        return;
    }
    snprintf(name, sizeof name, "%s", rel);
    (void)wl_net_stream_send_file(path, name);
}

WlResult wl_net_stream_watch_files(const char *root, const char *dirs) {
    if (root == NULL || strlen(root) >= sizeof g.watch_root || (dirs != NULL && strlen(dirs) >= sizeof g.watch_dirs)) {
        return WL_ERR_ARG;
    }
    if (!atomic_load(&g.open)) {
        return WL_ERR_STATE;
    }
    snprintf(g.watch_root, sizeof g.watch_root, "%s", root);
    snprintf(g.watch_dirs, sizeof g.watch_dirs, "%s", dirs != NULL ? dirs : "");
    wl_platform_fs_set_written_hook(on_written, NULL);
    return WL_OK;
}

// ---- the thread ----------------------------------------------------------------------------------

static bool stopping(void) {
    return atomic_load(&g.abandon);
}

// Sleeps `ms` in small steps that notice an abandon.
static void nap(uint32_t ms) {
    while (ms > 0u && !stopping()) {
        uint32_t step = ms < 20u ? ms : 20u;
        wl_platform_thread_sleep_ms(step);
        ms -= step;
    }
}

// One line about a failed step of getting connected, at most one every DIAG_MS (the thread retries
// every second: a log line for each would drown the run's own). It says which step and the code,
// and how many attempts failed since the last line. The line goes to the console log, and to the
// PC once there is a connection.
static void diag(const char *step, WlResult code) {
    static uint64_t last;
    static uint32_t skipped;
    uint64_t now = wl_platform_time_ms();

    if (last != 0u && now - last < DIAG_MS) {
        skipped++;
        return;
    }
    WL_LOG_W(TAG, "%s failed: %s (%d); %u more attempts failed since the last note", step, wl_net_result_name(code),
             (int)code, (unsigned)skipped);
    skipped = 0;
    last = wl_platform_time_ms();
    if (last == 0u) {
        last = 1u;
    }
}

static void disconnect(void) {
    if (g.sock != WL_SOCKET_INVALID) {
        (void)wl_net_close(g.sock);
        g.sock = WL_SOCKET_INVALID;
    }
    atomic_store(&g.connected, false);
}

// Sends all of `data`, waiting for room at most WL_NET_STREAM_STALL_MS without progress.
static bool send_all(const void *data, uint32_t size) {
    const uint8_t *p = data;
    uint64_t progress = wl_platform_time_ms();

    while (size > 0u) {
        int32_t n = wl_net_send(g.sock, p, size);
        if (n > 0) {
            p += n;
            size -= (uint32_t)n;
            atomic_fetch_add(&g.bytes_sent, (uint32_t)n);
            progress = wl_platform_time_ms();
            continue;
        }
        if (n != WL_ERR_AGAIN && n != 0) {
            return false;
        }
        if (stopping() || wl_platform_time_ms() - progress > WL_NET_STREAM_STALL_MS) {
            return false;
        }
        WlNetPoll item = {.socket = g.sock, .events = WL_NET_POLL_WRITE, .revents = 0};
        // (a poll that fails is not a dead connection, the send says that: pace it, do not spin)
        if (wl_net_poll(&item, 1u, 50) < 0) {
            wl_platform_thread_sleep_ms(20);
        } else if ((item.revents & (WL_NET_POLL_ERROR | WL_NET_POLL_HUP)) != 0u) {
            return false;
        }
    }
    return true;
}

static bool send_text(const char *text) {
    return send_all(text, (uint32_t)strlen(text));
}

static void quit_requested(void) {
    WL_LOG_I(TAG, "QUIT from the peer: leaving for the loader");
    g.in_sender_exit = true;
    if (g.quit_fn != NULL) {
        g.quit_fn();
    } else {
        wl_platform_exit(WL_EXIT_LOADER);
    }
    g.in_sender_exit = false; // (a quit handler that returned: the test's)
}

// What the PC says: lines, of which only QUIT is understood. False when the peer is gone.
static bool read_peer(void) {
    static char line[16];
    static uint32_t fill;
    char in[32];

    for (;;) {
        int32_t n = wl_net_recv(g.sock, in, sizeof in);
        if (n == WL_ERR_AGAIN) {
            return true;
        }
        if (n <= 0) {
            fill = 0;
            return false;
        }
        for (int32_t i = 0; i < n; i++) {
            if (in[i] == '\n') {
                line[fill < sizeof line ? fill : sizeof line - 1u] = '\0';
                bool quit = strcmp(line, "QUIT") == 0 || strcmp(line, "QUIT\r") == 0;
                fill = 0;
                if (quit && !g.quit_seen) {
                    g.quit_seen = true;
                    quit_requested();
                }
            } else if (fill < sizeof line - 1u) {
                line[fill++] = in[i];
            }
        }
    }
}

static bool send_file_job(const FileJob *job) {
    WlPlatformFile *file = NULL;
    char header[160];
    char *buf = g.buffer + CHUNK_BYTES;

    if (wl_platform_fs_open(job->path, WL_FILE_READ, &file) != WL_OK) {
        atomic_fetch_add(&g.files_dropped, 1u);
        return true;
    }
    uint64_t size = wl_platform_fs_size(file);
    if (size == 0u || size > WL_NET_STREAM_FILE_MAX) {
        wl_platform_fs_close(file);
        atomic_fetch_add(&g.files_dropped, 1u);
        return true;
    }
    snprintf(header, sizeof header, "FILE %s %u\n", job->name, (unsigned)size);
    bool ok = send_text(header);
    for (uint64_t at = 0; ok && at < size;) {
        uint32_t n = size - at < FILE_CHUNK ? (uint32_t)(size - at) : FILE_CHUNK;
        ok = wl_platform_fs_read_at(file, at, buf, n) == WL_OK && send_all(buf, n);
        at += n;
    }
    wl_platform_fs_close(file);
    if (ok) {
        atomic_fetch_add(&g.files_sent, 1u);
    } else {
        atomic_fetch_add(&g.files_dropped, 1u); // (the framing is broken: the caller drops the connection)
    }
    return ok;
}

// One round of sending. Returns true when it did something (so the thread goes round again at
// once), and sets *broken when the connection must go.
static bool pump(bool *broken) {
    if (!read_peer()) {
        *broken = true;
        return false;
    }
    uint32_t lines = 0;
    uint32_t n = ring_take(g.buffer, CHUNK_BYTES, &lines);
    if (n > 0u) {
        if (!send_all(g.buffer, n)) {
            atomic_fetch_add(&g.lines_lost, lines);
            g.lost_note += lines;
            *broken = true;
            return false;
        }
        atomic_fetch_add(&g.lines_sent, lines);
        return true;
    }
    FileJob job;
    if (file_pop(&job)) {
        if (!send_file_job(&job)) {
            *broken = true;
            return false;
        }
        return true;
    }
    return false;
}

static void say_hello(void) {
    char text[128];

    snprintf(text, sizeof text, "HELLO WLNET1 %s\n", g.run);
    bool ok = send_text(text);
    if (ok && g.lost_note > 0u) {
        snprintf(text, sizeof text, "[W][" TAG "] %u lines lost with the last connection\n", (unsigned)g.lost_note);
        g.lost_note = 0;
        ok = send_text(text);
    }
    if (!ok) {
        disconnect();
    }
}

static bool connected_now(void) {
    atomic_store(&g.connected, true);
    atomic_fetch_add(&g.connects, 1u);
    (void)wl_net_set_option(g.sock, WL_NET_OPT_NODELAY, 1);
    say_hello();
    return g.sock != WL_SOCKET_INVALID;
}

// Waits for the socket to poll ready for `events`, in steps that notice an abandon.
static bool wait_socket(WlSocket s, uint32_t events, uint32_t ms) {
    for (uint32_t waited = 0; waited < ms && !stopping(); waited += 50u) {
        WlNetPoll item = {.socket = s, .events = events, .revents = 0};
        int32_t r = wl_net_poll(&item, 1u, 50);
        if (r < 0) {
            return false;
        }
        if (r > 0) {
            return true;
        }
    }
    return false;
}

static bool acquire_listen(void) {
    if (g.listener == WL_SOCKET_INVALID) {
        WlNetAddr any = {.address = WL_NET_ADDR_ANY, .port = g.port};
        WlSocket s;
        WlResult made = wl_net_socket(WL_NET_TCP, &s);
        if (made != WL_OK) {
            diag("listen: socket", made);
            nap(RETRY_MS);
            return false;
        }
        (void)wl_net_set_option(s, WL_NET_OPT_REUSEADDR, 1);
        WlResult r = wl_net_bind(s, &any);
        const char *step = "listen: bind";
        if (r == WL_OK) {
            step = "listen";
            r = wl_net_listen(s, 1);
        }
        if (r == WL_OK) {
            step = "listen: non-blocking";
            r = wl_net_set_nonblocking(s, true);
        }
        if (r != WL_OK) {
            diag(step, r);
            (void)wl_net_close(s);
            nap(RETRY_MS);
            return false;
        }
        g.listener = s;
        WL_LOG_I(TAG, "listening on port %u", (unsigned)g.port);
    }
    // (the poll only paces the loop: the listener is non-blocking, so asking to accept is cheap and
    // does not rest on a poll that reports a connection to accept)
    (void)wait_socket(g.listener, WL_NET_POLL_READ, 100u);
    WlSocket c;
    WlNetAddr peer;
    WlResult accepted = wl_net_accept(g.listener, &c, &peer);
    if (accepted != WL_OK) {
        if (accepted != WL_ERR_AGAIN) {
            diag("accept", accepted);
        }
        return false;
    }
    if (wl_net_set_nonblocking(c, true) != WL_OK) {
        (void)wl_net_close(c);
        return false;
    }
    g.sock = c;
    return connected_now();
}

static bool connect_pending(WlResult r) {
    return r == WL_ERR_NET_INPROGRESS || r == WL_ERR_NET_ALREADY || r == WL_ERR_AGAIN;
}

// Waits for a non-blocking connect to finish, for CONNECT_MS. Sockets say how it went in two ways,
// and the thread takes whichever answers: the socket polls writable and wl_net_connect_result says
// how it ended (a host's SO_ERROR), or connecting again says it (the BSD idiom, and the one IOS
// gives for certain: EALREADY or EINPROGRESS while it goes on, EISCONN when it is done, the error
// when it failed; Dolphin follows IOS). So, besides the poll, a second connect is tried every
// PROBE_MS. Returns WL_OK when connected, else why not.
static WlResult finish_connect(WlSocket s, const WlNetAddr *to) {
    enum { PROBE_MS = 200 };
    uint32_t since_probe = PROBE_MS; // (the first probe comes with the first round)
    WlResult poll_failed = WL_OK;

    for (uint32_t waited = 0; waited < CONNECT_MS && !stopping(); waited += 50u) {
        WlNetPoll item = {.socket = s, .events = WL_NET_POLL_WRITE, .revents = 0};
        int32_t ready = wl_net_poll(&item, 1u, 50);

        if (ready < 0) {
            poll_failed = (WlResult)ready;
            wl_platform_thread_sleep_ms(50); // (a poll that fails answers at once: do not spin)
        } else if (ready > 0) {
            WlResult outcome;
            if (wl_net_connect_result(s, &outcome) != WL_OK) {
                outcome = WL_ERR_NET_REFUSED;
            }
            if (!connect_pending(outcome)) {
                return outcome;
            }
        }
        since_probe += 50u;
        if (since_probe >= PROBE_MS) {
            since_probe = 0;
            WlResult again = wl_net_connect(s, to);
            if (again == WL_OK || again == WL_ERR_NET_ISCONN) {
                return WL_OK;
            }
            if (!connect_pending(again)) {
                return again;
            }
        }
    }
    if (poll_failed != WL_OK) {
        diag("connect: poll", poll_failed);
    }
    return WL_ERR_NET_TIMEDOUT;
}

static bool acquire_connect(void) {
    uint32_t address;
    WlSocket s;
    WlResult r;

    if ((r = wl_net_resolve(g.host, &address)) != WL_OK) {
        diag("connect: resolve", r);
        nap(RETRY_MS);
        return false;
    }
    if ((r = wl_net_socket(WL_NET_TCP, &s)) != WL_OK) {
        diag("connect: socket", r);
        nap(RETRY_MS);
        return false;
    }
    if ((r = wl_net_set_nonblocking(s, true)) != WL_OK) {
        diag("connect: non-blocking", r);
        (void)wl_net_close(s);
        nap(RETRY_MS);
        return false;
    }
    WlNetAddr to = {.address = address, .port = g.port};
    r = wl_net_connect(s, &to);
    if (r == WL_ERR_NET_ISCONN) {
        r = WL_OK;
    } else if (connect_pending(r)) {
        r = finish_connect(s, &to);
        if (r != WL_OK) {
            diag("connect: waiting", r);
        }
    } else if (r != WL_OK) {
        diag("connect", r);
    }
    if (r != WL_OK) {
        (void)wl_net_close(s);
        nap(RETRY_MS);
        return false;
    }
    g.sock = s;
    return connected_now();
}

// The thread. The network first (DHCP takes seconds on a console), then connect, send, repeat.
static void *sender_main(void *arg) {
    (void)arg;
    uint64_t began = wl_platform_time_ms();
    uint64_t noted = began;
    WlResult started = wl_net_start();
    if (started != WL_OK) {
        diag("network start", started);
    }
    while (!stopping()) {
        WlResult up = wl_net_wait(200u);
        if (up == WL_OK) {
            uint32_t ip = wl_net_local_address();
            WL_LOG_I(TAG, "network up after %u ms, this console is %u.%u.%u.%u", (unsigned)(wl_platform_time_ms() - began),
                     (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 255u), (unsigned)((ip >> 8) & 255u), (unsigned)(ip & 255u));
            break;
        }
        if (up == WL_ERR_NET_DOWN) {
            diag("network", up);
            nap(3000u); // (no cable, no access point, no DHCP answer: try again)
            (void)wl_net_start();
        } else if (wl_platform_time_ms() - noted >= DIAG_MS) {
            noted = wl_platform_time_ms();
            WL_LOG_I(TAG, "waiting for the network: %u ms so far", (unsigned)(noted - began));
        }
    }
    while (!stopping()) {
        if (g.sock == WL_SOCKET_INVALID) {
            if (!(g.listen ? acquire_listen() : acquire_connect())) {
                continue;
            }
        }
        bool broken = false;
        bool worked = pump(&broken);
        if (broken) {
            disconnect();
            continue;
        }
        if (!worked) {
            if (atomic_load(&g.closing) && ring_empty() && !file_pending()) {
                (void)send_text("END\n");
                break;
            }
            nap(20u);
        }
    }
    disconnect();
    if (g.listener != WL_SOCKET_INVALID) {
        (void)wl_net_close(g.listener);
        g.listener = WL_SOCKET_INVALID;
    }
    atomic_store(&g.done, true);
    return NULL;
}

// ---- open and close ------------------------------------------------------------------------------

static void shutdown_hook(WlExitTarget target, void *user) {
    (void)target;
    (void)user;
    wl_net_stream_close(SHUTDOWN_FLUSH_MS);
}

WlResult wl_net_stream_open(const char *spec, const char *run_name) {
    if (spec == NULL) {
        return WL_ERR_ARG;
    }
    if (atomic_load(&g.open)) {
        return WL_ERR_STATE;
    }
    bool listen = strncmp(spec, "listen:", 7) == 0;
    const char *colon = listen ? spec + 6 : strrchr(spec, ':');
    if (colon == NULL || colon[1] == '\0' || (!listen && (colon == spec || (size_t)(colon - spec) >= sizeof g.host))) {
        return WL_ERR_FORMAT;
    }
    char *end;
    unsigned long port = strtoul(colon + 1, &end, 10);
    if (*end != '\0' || port == 0u || port > 65535u) {
        return WL_ERR_FORMAT;
    }
    g.listen = listen;
    g.port = (uint16_t)port;
    g.host[0] = '\0';
    if (!listen) {
        memcpy(g.host, spec, (size_t)(colon - spec));
        g.host[colon - spec] = '\0';
    }
    snprintf(g.run, sizeof g.run, "%s", run_name != NULL && run_name[0] != '\0' ? run_name : "run");
    for (char *c = g.run; *c != '\0'; c++) {
        bool plain = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '.' ||
                     *c == '_' || *c == '-';
        if (!plain) {
            *c = '_';
        }
    }
    // (the ring, the lock and the thread's buffer are made once and kept: a log line on another
    // thread may be in the sink while the stream closes)
    if (!g.lock_made) {
        if (wl_platform_mutex_create(&g.lock) != WL_OK) {
            return WL_ERR_FAILED;
        }
        g.lock_made = true;
    }
    if (g.ring == NULL) {
        g.ring = malloc(RING_BYTES);
    }
    if (g.buffer == NULL) {
        g.buffer = malloc(CHUNK_BYTES + FILE_CHUNK);
    }
    g.stack_in_mem = wl_mem_ready();
    g.stack = g.stack_in_mem ? WL_MEM_ALLOC_GPU(WL_TAG_STACK, STACK_BYTES) : aligned_alloc(32, STACK_BYTES);
    if (g.ring == NULL || g.buffer == NULL || g.stack == NULL) {
        goto fail;
    }
    g.head = g.tail = g.pending_drop = 0;
    g.file_head = g.file_tail = 0;
    g.lost_note = 0;
    g.sock = g.listener = WL_SOCKET_INVALID;
    g.in_sender_exit = false;
    g.quit_seen = false;
    g.joined = false;
    atomic_store(&g.closing, false);
    atomic_store(&g.abandon, false);
    atomic_store(&g.done, false);
    atomic_store(&g.connected, false);
    atomic_store(&g.connects, 0u);
    atomic_store(&g.lines_sent, 0u);
    atomic_store(&g.lines_dropped, 0u);
    atomic_store(&g.lines_lost, 0u);
    atomic_store(&g.bytes_sent, 0u);
    atomic_store(&g.files_sent, 0u);
    atomic_store(&g.files_dropped, 0u);
    atomic_store(&g.open, true);
    atomic_store(&g_wl_net_stream_on, true); // (lines from here on are kept)
    if (wl_platform_thread_create(&g.thread, sender_main, NULL, g.stack, STACK_BYTES, WL_THREAD_PRIO_NET) != WL_OK) {
        atomic_store(&g_wl_net_stream_on, false);
        atomic_store(&g.open, false);
        goto fail;
    }
    (void)wl_platform_shutdown_hook(shutdown_hook, NULL);
    WL_LOG_I(TAG, "streaming the log to %s%s%u as \"%s\"", g.listen ? "listen:" : g.host, g.listen ? "" : ":", (unsigned)g.port,
             g.run);
    return WL_OK;
fail:
    if (g.stack != NULL) {
        if (g.stack_in_mem) {
            wl_mem_free(g.stack);
        } else {
            free(g.stack);
        }
        g.stack = NULL;
    }
    return WL_ERR_NOMEM;
}

bool wl_net_stream_active(void) {
    return atomic_load(&g.open);
}

void wl_net_stream_stats(WlNetStreamStats *out) {
    out->connected = atomic_load(&g.connected);
    out->connects = atomic_load(&g.connects);
    out->lines_sent = atomic_load(&g.lines_sent);
    out->lines_dropped = atomic_load(&g.lines_dropped);
    out->lines_lost = atomic_load(&g.lines_lost);
    out->bytes_sent = atomic_load(&g.bytes_sent);
    out->files_sent = atomic_load(&g.files_sent);
    out->files_dropped = atomic_load(&g.files_dropped);
}

void wl_net_stream_set_quit(void (*fn)(void)) {
    g.quit_fn = fn;
}

void wl_net_stream_close(uint32_t flush_ms) {
    if (!atomic_load(&g.open) || atomic_load(&g.closing)) {
        return;
    }
    WlNetStreamStats st;
    wl_net_stream_stats(&st);
    WL_LOG_I(TAG, "closing: %u lines sent, %u dropped, %u lost; %u files sent, %u dropped; %u connections",
             (unsigned)st.lines_sent, (unsigned)st.lines_dropped, (unsigned)st.lines_lost, (unsigned)st.files_sent,
             (unsigned)st.files_dropped, (unsigned)st.connects);
    wl_platform_fs_set_written_hook(NULL, NULL);
    atomic_store(&g.closing, true);
    uint64_t deadline = wl_platform_time_ms() + flush_ms;
    if (g.in_sender_exit) {
        // QUIT: the thread is the one leaving, so it flushes itself before the program goes
        bool broken = false;
        while (g.sock != WL_SOCKET_INVALID && !broken && wl_platform_time_ms() < deadline && pump(&broken)) {
        }
        if (g.sock != WL_SOCKET_INVALID && !broken) {
            (void)send_text("END\n");
        }
        return;
    }
    while (!atomic_load(&g.done) && wl_platform_time_ms() < deadline) {
        wl_platform_thread_sleep_ms(10);
    }
    atomic_store(&g.abandon, true);
    if (!g.joined) {
        g.joined = true;
        (void)wl_platform_thread_join(g.thread, NULL);
    }
    atomic_store(&g_wl_net_stream_on, false);
    atomic_store(&g.open, false);
    atomic_store(&g.closing, false);
    if (g.stack_in_mem) {
        wl_mem_free(g.stack);
    } else {
        free(g.stack);
    }
    g.stack = NULL;
}
