/*
 * PVGPU Kernel-Mode Driver Header
 *
 * WDDM 2.0 display miniport driver for paravirtualized GPU.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef PVGPU_KMD_H
#define PVGPU_KMD_H

#include <ntddk.h>
#include <dispmprt.h>
#include <d3dkmddi.h>
#include <d3dkmdt.h>

/* Include shared protocol */
#include "../../protocol/pvgpu_protocol.h"

/*
 * =============================================================================
 * Driver Constants
 * =============================================================================
 */

#define PVGPU_POOL_TAG          'UPGV'  /* "VGPU" backwards */
#define PVGPU_DRIVER_VERSION    0x0001

/*
 * =============================================================================
 * Device Context
 * =============================================================================
 */

typedef struct _PVGPU_DEVICE_CONTEXT {
    /* WDDM handles */
    HANDLE                      DeviceHandle;
    DXGKRNL_INTERFACE           DxgkInterface;
    DXGK_START_INFO             StartInfo;
    
    /* BAR0: Control registers */
    PHYSICAL_ADDRESS            Bar0PhysAddr;
    ULONG                       Bar0Length;
    volatile ULONG*             Bar0VirtAddr;
    
    /* BAR2: Shared memory */
    PHYSICAL_ADDRESS            Bar2PhysAddr;
    ULONG                       Bar2Length;
    volatile UCHAR*             Bar2VirtAddr;
    
    /* Control region pointer (start of BAR2) */
    volatile PvgpuControlRegion* ControlRegion;
    
    /* Command ring base (after control region) */
    volatile UCHAR*             CommandRing;
    ULONG                       CommandRingSize;
    
    /* Resource heap base (after command ring) */
    volatile UCHAR*             ResourceHeap;
    ULONG                       ResourceHeapSize;
    
    /* Interrupt state */
    ULONG                       InterruptMessageNumber;
    BOOLEAN                     InterruptEnabled;
    
    /* Display state */
    ULONG                       DisplayWidth;
    ULONG                       DisplayHeight;
    ULONG                       DisplayRefresh;
    
    /* Spinlock for command ring access */
    KSPIN_LOCK                  CommandLock;
    
} PVGPU_DEVICE_CONTEXT, *PPVGPU_DEVICE_CONTEXT;

/*
 * =============================================================================
 * WDDM Callback Prototypes
 * =============================================================================
 */

/* Driver Entry and Unload */
DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD DriverUnload;

/* Device Power and PnP */
NTSTATUS
PvgpuAddDevice(
    _In_ DEVICE_OBJECT* PhysicalDeviceObject,
    _Outptr_ PVOID* MiniportDeviceContext
);

NTSTATUS
PvgpuStartDevice(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGK_START_INFO* DxgkStartInfo,
    _In_ DXGKRNL_INTERFACE* DxgkInterface,
    _Out_ PULONG NumberOfVideoPresentSources,
    _Out_ PULONG NumberOfChildren
);

NTSTATUS
PvgpuStopDevice(
    _In_ PVOID MiniportDeviceContext
);

NTSTATUS
PvgpuRemoveDevice(
    _In_ PVOID MiniportDeviceContext
);

/* Interrupts */
BOOLEAN
PvgpuInterruptRoutine(
    _In_ PVOID MiniportDeviceContext,
    _In_ ULONG MessageNumber
);

VOID
PvgpuDpcRoutine(
    _In_ PVOID MiniportDeviceContext
);

/* Display Functions */
NTSTATUS
PvgpuQueryAdapterInfo(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_QUERYADAPTERINFO* QueryAdapterInfo
);

NTSTATUS
PvgpuQueryChildRelations(
    _In_ PVOID MiniportDeviceContext,
    _Inout_ DXGK_CHILD_DESCRIPTOR* ChildRelations,
    _In_ ULONG ChildRelationsSize
);

NTSTATUS
PvgpuQueryChildStatus(
    _In_ PVOID MiniportDeviceContext,
    _Inout_ DXGK_CHILD_STATUS* ChildStatus,
    _In_ BOOLEAN NonDestructiveOnly
);

NTSTATUS
PvgpuQueryDeviceDescriptor(
    _In_ PVOID MiniportDeviceContext,
    _In_ ULONG ChildUid,
    _Inout_ DXGK_DEVICE_DESCRIPTOR* DeviceDescriptor
);

NTSTATUS
PvgpuSetPowerState(
    _In_ PVOID MiniportDeviceContext,
    _In_ ULONG HardwareUid,
    _In_ DEVICE_POWER_STATE DevicePowerState,
    _In_ POWER_ACTION ActionType
);

/* Memory Management */
NTSTATUS
PvgpuBuildPagingBuffer(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_BUILDPAGINGBUFFER* BuildPagingBuffer
);

NTSTATUS
PvgpuSubmitCommand(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_SUBMITCOMMAND* SubmitCommand
);

NTSTATUS
PvgpuPreemptCommand(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_PREEMPTCOMMAND* PreemptCommand
);

NTSTATUS
PvgpuPatch(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_PATCH* Patch
);

/* GPU Scheduler */
NTSTATUS
PvgpuCreateDevice(
    _In_ PVOID MiniportDeviceContext,
    _Inout_ DXGKARG_CREATEDEVICE* CreateDevice
);

NTSTATUS
PvgpuDestroyDevice(
    _In_ PVOID MiniportDeviceContext,
    _In_ HANDLE DeviceHandle
);

NTSTATUS
PvgpuCreateContext(
    _In_ PVOID MiniportDeviceContext,
    _Inout_ DXGKARG_CREATECONTEXT* CreateContext
);

NTSTATUS
PvgpuDestroyContext(
    _In_ PVOID MiniportDeviceContext,
    _In_ HANDLE ContextHandle
);

/* Allocations */
NTSTATUS
PvgpuCreateAllocation(
    _In_ PVOID MiniportDeviceContext,
    _Inout_ DXGKARG_CREATEALLOCATION* CreateAllocation
);

NTSTATUS
PvgpuDestroyAllocation(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_DESTROYALLOCATION* DestroyAllocation
);

NTSTATUS
PvgpuDescribeAllocation(
    _In_ PVOID MiniportDeviceContext,
    _Inout_ DXGKARG_DESCRIBEALLOCATION* DescribeAllocation
);

NTSTATUS
PvgpuGetStandardAllocationDriverData(
    _In_ PVOID MiniportDeviceContext,
    _Inout_ DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA* StandardAllocation
);

/* Present */
NTSTATUS
PvgpuPresent(
    _In_ PVOID MiniportDeviceContext,
    _Inout_ DXGKARG_PRESENT* Present
);

/* Render (command buffer building) */
NTSTATUS
PvgpuRender(
    _In_ PVOID MiniportDeviceContext,
    _Inout_ DXGKARG_RENDER* Render
);

/*
 * =============================================================================
 * Internal Helper Functions
 * =============================================================================
 */

/* BAR access helpers */
FORCEINLINE ULONG
PvgpuReadBar0(
    _In_ PPVGPU_DEVICE_CONTEXT Context,
    _In_ ULONG Offset
)
{
    return READ_REGISTER_ULONG(&Context->Bar0VirtAddr[Offset / sizeof(ULONG)]);
}

FORCEINLINE VOID
PvgpuWriteBar0(
    _In_ PPVGPU_DEVICE_CONTEXT Context,
    _In_ ULONG Offset,
    _In_ ULONG Value
)
{
    WRITE_REGISTER_ULONG(&Context->Bar0VirtAddr[Offset / sizeof(ULONG)], Value);
}

/* Ring buffer submit */
NTSTATUS
PvgpuSubmitToRing(
    _In_ PPVGPU_DEVICE_CONTEXT Context,
    _In_ PVOID CommandData,
    _In_ ULONG CommandSize
);

/* Doorbell notification */
FORCEINLINE VOID
PvgpuRingDoorbell(
    _In_ PPVGPU_DEVICE_CONTEXT Context
)
{
    PvgpuWriteBar0(Context, PVGPU_REG_DOORBELL, 1);
}

#endif /* PVGPU_KMD_H */
