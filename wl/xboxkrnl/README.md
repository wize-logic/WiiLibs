# wl/xboxkrnl: the Xbox kernel for recompiled titles

Target `wl_xboxkrnl` (alias `WiiLibs::xboxkrnl`) depends on `wl_xboxrt` and `wl_platform`. It
implements the exports a title imports by ordinal from `xboxkrnl.exe`, over wl/xboxrt's guest
memory, threads and interrupts and wl/platform's files. It is portable C: guest structures are
read and written field by field through little-endian accessors, so it runs the same on the Wii,
the host and ppc-be. Header: `wl/xboxkrnl/xboxkrnl.h`.

```c
WlXkConfig kc;
wl_xk_config_default(&kc);
kc.title_dir = "sd:/xbox/halo";      // \Device\CdRom0, D:
kc.data_dir = "sd:/xbox/halo/hdd";   // the hard disk's partitions
kc.title_id = xbe.title_id;          // T: and U:
wl_xk_init(&kc);                     // after wl_xb_mem_init and wl_xb_threads_init
wl_xbe_resolve_thunks(&xbe, wl_xk_resolve, NULL, NULL, 0, false, &resolved);
```

`wl/xboxrt/boot.h`'s `wl_xb_boot_run` does all of this for a title (`kernel_config` edits the
settings).

## What it covers

- **Exports**: `exports.inc` lists the retail kernel's 371 exports, their names and stdcall argument
  bytes (as xboxrecomp records them). `wl_xk_resolve` binds a function export to its `k_<Name>` in
  this library, or binds it with no body so the thunk dispatcher logs its first call and returns 0.
  A data export (`KeTickCount`, `XboxHardwareInfo`, the object types, keys) is bound to its
  variable on the kernel's data page (`kdata.c`). `wl_xk_export_info` says which is which. The
  log line "kernel up: N of 371 exports implemented" gives the count.
- **Dispatcher objects in guest memory** (`ke_wait.c`, `ke.c`, `nt_sync.c`): events,
  semaphores, mutants, timers and threads, with their `DISPATCHER_HEADER`s where the title put them.
  Waits on one or many (any or all) with timeouts, alertable waits and user APCs,
  `NtUserIoApcDispatcher`, named objects, handles (`ob.c`: up to 4096, pool blocks with headers),
  `NtDuplicateObject`. An object lives while anything refers to it: a handle, a reference by
  pointer (`ObReferenceObjectByHandle`, `ObfReferenceObject`; `ObfDereferenceObject` finds the
  object by its body and gives one back) or a wait on it (a handle closed under a waiting thread
  leaves the body until the wait ends). The last reference frees it (a file is closed then). As on
  NT, a signal satisfies the waits it can at once, in the order they began (so a pulse, or a set and
  a reset before the waiter runs, still releases it); an alertable wait its objects satisfy returns
  their status before a queued APC; a thread that ends abandons the mutants it owns (the next waiter
  gets `STATUS_ABANDONED_WAIT_0`).
- **Critical sections** (`rtl.c`), with the layout XAPI's inlined fast paths use: the event, then
  LockCount at +0x10, RecursionCount at +0x14, OwningThread at +0x18.
- **IRQL, DPCs, timers, interrupts** (`ke_irq.c`): the IRQL lives in each thread's KPCR. Timers
  have deadlines on the runtime's service. `HalGetInterruptVector` gives vector `0x30 + line` at
  IRQL `26 - line`. `KeConnectInterrupt` makes the line's ISR run at its IRQL when the line is
  asserted. DPCs drain at DISPATCH_LEVEL. The work runs at the runtime's safe points (`wl/xboxrt/irq.h`);
  a thread at DISPATCH_LEVEL or above keeps the processor (no thread switch or yield, no DPC, no
  timer expiry) until it lowers its IRQL, as on the console, and a line its IRQL masks waits
  without running the handler.
- **Threads** (`ps.c`): `PsCreateSystemThreadEx` makes real guest threads (optionally the first
  runs on the caller, xboxrecomp's inline model), with priorities from `KeSetBasePriorityThread`
  (base 16 plus the increment), suspend before start and resume, `PsTerminateSystemThread`.
- **Memory** (`mm.c`): pools, contiguous memory (pinned physical ranges), system memory,
  `MmGetPhysicalAddress`, `MmClaimGpuInstanceMemory`, the `Nt*VirtualMemory` calls, statistics.
- **Files** (`path.c`, `nt_file.c`): see below.
- **Settings, AV, HAL** (`ex.c`, `hal.c`): `ExQueryNonVolatileSetting` from `WlXkConfig`
  (language, video and audio flags, regions, parental control, the time zone, and the 0x60-byte
  user section of `XC_MAX_OS`), `ExSaveNonVolatileSetting` kept in memory, the AV pack and the
  encoder options, `AvSetDisplayMode` passed to a hook (`wl_xk_set_display_hook`),
  `HalReturnToFirmware` ending the process, `XeLoadSection`/`XeUnloadSection` reference counts
  (every section stays resident and is never read again: one changed, unloaded and loaded again
  keeps its changes, where the console reads it again from the XBE; Halo loads and unloads none).
- **Rtl** (`rtl.c`): strings (ANSI and Unicode), memory, integers, time fields, NTSTATUS to Win32
  errors, and a printf engine over guest varargs for `DbgPrint`, `RtlSprintf`, `RtlSnprintf`,
  `RtlVsprintf` and `RtlVsnprintf`.
- **Crypto** (`xc.c`): SHA-1, HMAC-SHA-1, RC4 (state in the title's buffer), DES key parity.

## Files

Names resolve the way the console's object manager resolves them. A drive letter goes through its
symbolic link (`IoCreateSymbolicLink`, which XAPI calls at start-up), or through the default links
when none was made: D: is `\Device\CdRom0`, C: Partition2, E: Partition1, T: and U:
`Partition1\TDATA` and `\UDATA` with the title id, X:, Y:, Z: Partitions 3 to 5. The device then
resolves through the mount table:

| Device | Folder | |
|---|---|---|
| `\Device\CdRom0` | `title_dir` | read-only |
| `\Device\Harddisk0\Partition1` | `data_dir/E` | saves (TDATA, UDATA) |
| `\Device\Harddisk0\Partition2` | `data_dir/C` | |
| `\Device\Harddisk0\Partition3`..`5` | `data_dir/X`, `Y`, `Z` | the cache partitions, sparse |

`WlXkConfig.mounts` add or replace devices. Lookups are case-blind (FATX is case-insensitive; a
host's file system need not be), and `..` above a mount is refused. Each look costs a walk from the
volume's root on libfat, so a name that is not there (a file about to be created) is looked for in
another case only in its folder's listing when the folder is there as written, and not at all on a
volume that folds the case itself (FAT: probed once per mount at start, "folds case" in its log line). Covered: `NtCreateFile`,
`NtOpenFile` and `IoCreateFile` with the six dispositions and delete-on-close; reads and writes at
offsets or at the file position, overlapped ones completing at once (their event, the file object,
the APC; `STATUS_PENDING` first with `async_io_pending`; both events are cleared before the request,
so one that fails leaves them non-signalled and sets neither the status block nor the APC, as NT);
writes only through a handle opened for writing; the query and set information classes
titles use (queries: basic, standard, internal, position, mode, network open; sets: basic,
rename, disposition, position, allocation, end of file, mode);
volume information (volume, size, device, full size); directory listings with wildcards, one entry per call; `NtDeleteFile`;
`NtFlushBuffersFile`. The handles on one file share a record, as NT's file control block: a delete
asked for (delete-on-close, the disposition, `NtDeleteFile` of an open file) happens at its last
close, and meanwhile every open or create of the name fails with `STATUS_DELETE_PENDING`. A rename
moves every handle on the file to the new name; one that would replace an open file is
`STATUS_ACCESS_DENIED`. On a sparse partition a file keeps a logical size beyond what was written, in a
`<name>.wlsz` sidecar that listings hide; its handles share the size, and only one that changed it
writes the sidecar (none is removed while the kernel knows there is none: its open found none, and
`wl_xk_file_changed` has not been called since). A partition opened as a device (`Partition0`, the cache partitions for
`XMountUtilityDrive`) keeps its first 64 KB in `data_dir/raw<N>.bin`; `Partition0` shows a
partition table in sector 0 (the homebrew `****PARTINFO****`), and sector 4 is XAPI's own (its
utility drive's cache database, which it reads back at every boot). Device controls answer the
geometry, partition information and the DVD media check.

## Halo's imports

Every one of the 134 imports in Halo's XBE resolves to a body or a data export (`yes`: implemented;
`data`: a variable on the data page; `stub` and `approx` are in the table further down).

| File | Exports (ordinal) |
|---|---|
| `hal.c` | AvGetSavedDataAddress (1), AvSendTVEncoderOption (2), AvSetDisplayMode (3), AvSetSavedDataAddress (4), HalRegisterShutdownNotification (47, approx), HalReturnToFirmware (49), PhyGetLinkState (252, stub), PhyInitialize (253), XeLoadSection (327), XeUnloadSection (328), HalIsResetOrShutdownPending (358, stub), HalInitiateShutdown (360) |
| `ke.c` | KeBugCheck (95), KeDelayExecutionThread (99), KeQueryInterruptTime (125), KeQueryPerformanceCounter (126), KeQueryPerformanceFrequency (127), KeQuerySystemTime (128), KeRestoreFloatingPointState (139, approx), KeSaveFloatingPointState (142, approx), KeSetEvent (145), KeStallExecutionProcessor (151), KeWaitForMultipleObjects (158), KeWaitForSingleObject (159) |
| `ke_irq.c` | HalGetInterruptVector (44), KeCancelTimer (97), KeConnectInterrupt (98), KeDisconnectInterrupt (100), KeInitializeDpc (107), KeInitializeInterrupt (109), KeInitializeTimerEx (113), KeInsertQueueDpc (119), KeRaiseIrqlToDpcLevel (129), KeRemoveQueueDpc (137), KeSetTimer (149), KeSetTimerEx (150), KeSynchronizeExecution (153), KfRaiseIrql (160), KfLowerIrql (161) |
| `mm.c` | ExAllocatePool (14), ExAllocatePoolWithTag (15), ExFreePool (17), ExQueryPoolBlockSize (23), MmAllocateContiguousMemory (165), MmAllocateContiguousMemoryEx (166), MmAllocateSystemMemory (167), MmClaimGpuInstanceMemory (168), MmFreeContiguousMemory (171), MmFreeSystemMemory (172), MmGetPhysicalAddress (173), MmLockUnlockBufferPages (175), MmLockUnlockPhysicalPage (176), MmPersistContiguousMemory (178, approx), MmQueryAddressProtect (179), MmQueryAllocationSize (180), MmQueryStatistics (181), MmSetAddressProtect (182), NtAllocateVirtualMemory (184), NtFreeVirtualMemory (199), NtQueryVirtualMemory (217) |
| `nt_file.c` | IoCreateSymbolicLink (67), NtCreateFile (190), NtDeviceIoControlFile (196), NtFlushBuffersFile (198), NtFsControlFile (200, approx), NtOpenFile (202), NtOpenSymbolicLinkObject (203), NtQueryDirectoryFile (207), NtQueryFullAttributesFile (210), NtQueryInformationFile (211), NtQuerySymbolicLinkObject (215), NtQueryVolumeInformationFile (218), NtReadFile (219), NtSetInformationFile (226), NtWriteFile (236) |
| `nt_sync.c` | NtClearEvent (186), NtClose (187), NtCreateEvent (189), NtCreateMutant (192), NtReleaseMutant (221), NtSetEvent (225), NtUserIoApcDispatcher (232), NtWaitForSingleObject (233), NtWaitForSingleObjectEx (234), NtWaitForMultipleObjectsEx (235) |
| `ps.c` | KeSetBasePriorityThread (143), NtResumeThread (224), NtYieldExecution (238), PsCreateSystemThreadEx (255), PsTerminateSystemThread (258) |
| `rtl.c` | DbgPrint (8), RtlCompareMemoryUlong (269), RtlEnterCriticalSection (277), RtlEqualString (279), RtlInitAnsiString (289), RtlInitializeCriticalSection (291), RtlLeaveCriticalSection (294), RtlNtStatusToDosError (301), RtlRaiseException (302, approx), RtlTimeFieldsToTime (304), RtlTimeToTimeFields (305), RtlUnicodeStringToAnsiString (308), RtlUnwind (312, approx) |
| `ex.c` | ExQueryNonVolatileSetting (24) |
| `io.c` | IoCreateDevice (65), IoInvalidDeviceRequest (74), IoStartNextPacket (81, stub), IoStartPacket (83, stub), IofCompleteRequest (87, stub), IoMarkIrpMustComplete (359, stub) |
| `ob.c` | ObReferenceObjectByHandle (246), ObfDereferenceObject (250) |
| `xc.c` | XcSHAInit (335), XcSHAUpdate (336), XcSHAFinal (337), XcRC4Key (338), XcRC4Crypt (339), XcHMAC (340), XcModExp (345, stub), XcDESKeyParity (346), XcKeyTable (347, stub), XcBlockCryptCBC (349, stub) |
| `kdata.c` (data) | ExEventObjectType (16), HalDiskCachePartitionCount (40), HalDiskModelNumber (41), HalDiskSerialNumber (42), HalBootSMCVideoMode (356), KeTickCount (156), LaunchDataPage (164), PsThreadObjectType (259), XboxHardwareInfo (322), XboxHDKey (323), XboxSignatureKey (325), XboxLANKey (353) |

Stubs and approximations among them:

| Export | What it does here |
|---|---|
| HalIsResetOrShutdownPending (358) | 0: nothing is ever pending |
| PhyGetLinkState (252) | no link (the network stays down; XNet fails cleanly) |
| IoStartPacket, IoStartNextPacket, IofCompleteRequest, IoMarkIrpMustComplete (83, 81, 87, 359) | no IRP processing: no driver of a title's own runs |
| XcModExp, XcKeyTable, XcBlockCryptCBC (345, 347, 349) | logged stubs that do nothing (RSA, DES): no title data is signed or encrypted with them here |
| KeSaveFloatingPointState, KeRestoreFloatingPointState (142, 139) | success without saving (the x87 state is the thread's register file) |
| MmPersistContiguousMemory (178) | success: nothing persists across a reboot |
| HalRegisterShutdownNotification (47) | ignored: nothing shuts down through it |
| RtlRaiseException (302) | logs the exception; no SEH dispatch |
| RtlUnwind (312) | unlinks the `fs:[0]` chain to the target frame without calling the handlers |
| NtFsControlFile (200) | success (FATX volume controls) |
| KeBugCheck (95) | a crash report with the code, then the stop |
| MmClaimGpuInstanceMemory (168) | returns the end of the kept 64 KB (`gpu_instance_end`, default true). Which one D3D expects is unverified: xboxrecomp returns the start, and the knob switches to that |

## Assumptions to know

- The AV option numbers and the capability word follow xboxrecomp's numbering (option 6 gives the
  AV pack, the standard in bits 8 and up, 60 Hz).
- `ETHREAD.UniqueThread` is at KTHREAD + 0x134. The `XC_MAX_OS` user section's layout comes from
  public headers as remembered and is not checked against a console.
- `KeQueryPerformanceFrequency` is 3 375 000 Hz, `KeTimeIncrement` 1 ms in 100 ns units.
- DirectSound finds a ready AC'97 codec and a running APU when wl/xboxrt attaches `wl/xboxapu`'s models (the default); with the idle devices (`noaudio`, `WL_XB_AUDIO=0`) the codec is not ready and DirectSound fails to start.

## Tests

`tests/xboxkrnl/test_xboxkrnl.c` builds a synthetic XBE in C and calls the imports the way
translated code does, from guest functions written in C (manual lookup). It covers the thunk ABI
(stdcall pops, fastcall, 64-bit returns); four threads contending for a critical section; events,
timeouts and mutants; timers with DPCs; an interrupt line with its ISR and DPC; an ISR and a thread
switch delivered to a loop that only polls a device register (D3D's fence wait); overlapped reads
with their APC, and a failed one that leaves its events cleared; the read-only DVD, a sparse cache drive and
its size shared by handles, listings (a buffer too small, "*.*"), delete at the last close (opens refused meanwhile), a rename that moves every handle, a full
card, read-only handles; objects kept by a reference by pointer and by a wait; virtual and
contiguous memory and the GPU instance, a commit with no address, decommits; Rtl strings, time and
printf past 2 KB; SHA-1, HMAC (RFC 2202) and
RC4; the EEPROM settings. It runs on the host and on ppc-be, and again in the runtime's deterministic mode (`test_xboxkrnl_det`). `tests/xboxrt/e2e/` runs a lifted title on
the kernel.

Provenance: `exports.inc`, `rtl.c`, `kdata.c`, `hal.c`, `ex.c`, `path.c` and `nt_file.c` derive in
part from xboxrecomp (MIT); `third_party/README.md` says what was taken.
