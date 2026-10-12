// SPDX-License-Identifier: MIT
// Io exports beyond files: device objects (made in the pool, as a driver would expect, and never
// sent anything), the IRP calls (no driver stack here: logged stubs that complete), dismounts.
#include "wl/xboxkrnl/kernel_internal.h"

#define DEVICE_OBJECT_BYTES 0x40u // Type, Size, ReferenceCount, DriverObject, ... (zeroed)

// NTSTATUS IoCreateDevice(PDRIVER_OBJECT DriverObject, ULONG DeviceExtensionSize,
//     POBJECT_STRING DeviceName, DEVICE_TYPE DeviceType, BOOLEAN Exclusive, PDEVICE_OBJECT *DeviceObject)
void k_IoCreateDevice(void) {
    uint32_t ext = ARG(1), out = ARG(5);
    uint32_t dev = xk_pool_alloc(DEVICE_OBJECT_BYTES + ext);
    if (dev == 0) {
        RET(STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    wl_xb_wr16(dev + 0u, 3);                      // IO_TYPE_DEVICE
    wl_xb_wr16(dev + 2u, (uint16_t)DEVICE_OBJECT_BYTES);
    wl_xb_wr32(dev + 8u, ARG(0));                  // DriverObject
    wl_xb_wr32(dev + 0x18u, ext != 0 ? dev + DEVICE_OBJECT_BYTES : 0u); // DeviceExtension
    wl_xb_wr32(dev + 0x1Cu, ARG(3));               // DeviceType
    char name[96];
    xk_read_ansi(ARG(2), name, sizeof name);
    WL_LOG_I(XK_TAG, "IoCreateDevice \"%s\" type 0x%x -> 0x%08x", name, (unsigned)ARG(3), (unsigned)dev);
    if (out != 0 && wl_xb_range_ok(out, 4)) {
        wl_xb_wr32(out, dev);
    }
    RET(STATUS_SUCCESS);
}

void k_IoDeleteDevice(void) {
    xk_pool_free(ARG(0));
}

// NTSTATUS IoInvalidDeviceRequest(PDEVICE_OBJECT, PIRP)
void k_IoInvalidDeviceRequest(void) {
    RET(STATUS_INVALID_DEVICE_REQUEST);
}

void k_IoStartNextPacket(void) {
    xk_stub("IoStartNextPacket");
}

void k_IoStartPacket(void) {
    xk_stub("IoStartPacket");
}

// VOID FASTCALL IofCompleteRequest(PIRP Irp, CCHAR PriorityBoost)
void k_IofCompleteRequest(void) {
    xk_stub("IofCompleteRequest");
}

void k_IoMarkIrpMustComplete(void) {
    xk_stub("IoMarkIrpMustComplete");
}

// NTSTATUS FASTCALL IofCallDriver(PDEVICE_OBJECT, PIRP): no driver answers
void k_IofCallDriver(void) {
    xk_stub("IofCallDriver");
    RET(STATUS_INVALID_DEVICE_REQUEST);
}

void k_IoDismountVolume(void) {
    RET(STATUS_SUCCESS);
}

void k_IoDismountVolumeByName(void) {
    RET(STATUS_SUCCESS);
}

void xk_link_io(void) {
}
