// SPDX-License-Identifier: MIT
// The file layer's internals (path.c, nt_file.c, io.c): mounts, resolved names, file objects.
#pragma once

#include "wl/xboxkrnl/kernel_internal.h"

typedef struct {
    char device[64];  // "\Device\Harddisk0\Partition1"
    char host[192];   // "sd:/xbox/halo/hdd/E"
    bool writable;
    bool sparse;
    bool folds_case;  // the volume finds a name in another case itself (FAT): no listing for a miss
} XkMount;

typedef enum {
    XK_RES_HOST = 0,  // a file or folder under a mount
    XK_RES_CDROM,     // the DVD drive itself
    XK_RES_PARTITION, // a disk partition (or the whole disk, 0) itself
} XkResKind;

typedef struct {
    XkResKind kind;
    XkMount mount;          // XK_RES_HOST
    char host[320];         // the platform path
    uint32_t root_len;      // the mount root's length in `host`
    uint32_t partition;     // XK_RES_PARTITION
    char device_path[400];  // the name as a device path (for FileNameInformation)
} XkResolved;

void xk_paths_init(const WlXkConfig *cfg);
bool xk_resolve(const char *name, XkResolved *out);
bool xk_resolve_case(XkResolved *r, WlPlatformStat *st);
// For tests: every mount as on a volume that folds the case (`folds`) or tells the cases apart.
void xk_paths_set_folds_case(bool folds);
bool xk_link_delete(const char *link);

int xk_stricmp(const char *a, const char *b);
int xk_strnicmp(const char *a, const char *b, size_t n);
#define strcasecmp xk_stricmp
#define strncasecmp xk_strnicmp

typedef enum { XK_FILE_REGULAR = 0, XK_FILE_DIRECTORY, XK_FILE_CDROM, XK_FILE_PARTITION } XkFileKind;

// What every handle on one host file or folder shares (nt_file.c), as NT's file control block.
typedef struct XkFcb XkFcb;

struct XkFile {
    XkFileKind kind;
    WlPlatformFile *pf;     // regular files
    char host[320];
    char name[400];         // the device path it was opened by
    bool writable;          // the mount can be written
    bool can_write;         // ...and the handle asked for it (FILE_WRITE_DATA and the like)
    bool sparse;
    bool delete_on_close;   // FILE_DELETE_ON_CLOSE: the file is deleted after its last handle
    bool synchronous;       // FILE_SYNCHRONOUS_IO_*: no APC, the position moves
    bool resized;           // this handle changed a sparse file's size: it writes the sidecar
    XkFcb *fcb;             // files and folders under a mount
    XkFile *fcb_next;       // the other handles on the same file
    uint64_t pos;
    uint32_t partition;     // XK_FILE_PARTITION
    WlPlatformDir *dir;     // a directory being listed
    char pattern[128];
    bool listing;
    uint32_t listed;        // entries returned since the restart
    bool held;              // an entry found that did not fit the caller's buffer: returned next
    char held_name[256];
    WlPlatformStat held_st;
    // what this handle moved (a map's load is mostly this): logged every XK_IO_LOG_BYTES and at close
    uint32_t io_reads, io_writes;
    uint32_t io_jumps;       // requests that did not start where the last one ended
    uint64_t io_read_bytes, io_write_bytes;
    uint64_t io_us;          // the time spent in the card's reads and writes
    uint64_t io_end;         // where the last request ended
    uint64_t io_logged;      // the bytes at the last log line
};

// Raw partition sectors (the partition table on Partition0; what the title wrote to a partition's
// first 64 KB, kept in data_dir).
void xk_raw_read(uint32_t partition, uint64_t offset, uint8_t *dst, uint32_t n);
void xk_raw_write(uint32_t partition, uint64_t offset, const uint8_t *src, uint32_t n);
uint64_t xk_raw_size(uint32_t partition);
