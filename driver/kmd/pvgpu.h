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
00024|  * =============================================================================
00025|  * Driver Constants
00026|  * =============================================================================
00027|  */

#define PVGPU_POOL_TAG          'UPGV'  /* "VGPU" backwards */
#define PVGPU_DRIVER_VERSION    0x0001

/* Heap allocator constants */
#define PVGPU_HEAP_BLOCK_SIZE   0x1000      /* 4KB minimum block */
#define PVGPU_HEAP_MAX_BLOCKS   4096        /* Max blocks (for 16MB heap at 4KB blocks) */

/* Display mode constants */
#define PVGPU_MAX_DISPLAY_MODES     16      /* Maximum supported display modes */
#define PVGPU_DEFAULT_REFRESH_RATE  60      /* Default refresh rate in Hz */

/*
 * =============================================================================
 * Display Mode Definition
 * =============================================================================
 */

typedef struct _PVGPU_DISPLAY_MODE {
    ULONG   Width;              /* Horizontal resolution */
    ULONG   Height;             /* Vertical resolution */
    ULONG   RefreshRate;        /* Refresh rate in Hz */
    BOOLEAN Active;             /* Mode is currently active */
} PVGPU_DISPLAY_MODE, *PPVGPU_DISPLAY_MODE;

/* Standard display modes table */
static const PVGPU_DISPLAY_MODE g_DisplayModes[] = {
    /* 16:9 resolutions */
    { 1280,  720,  60, FALSE },     /* 720p */
    { 1280,  720, 120, FALSE },
    { 1920, 1080,  60, FALSE },     /* 1080p */
    { 1920, 1080, 120, FALSE },
    { 1920, 1080, 144, FALSE },
    { 2560, 1440,  60, FALSE },     /* 1440p */
    { 2560, 1440, 120, FALSE },
    { 2560, 1440, 144, FALSE },
    { 3840, 2160,  60, FALSE },     /* 4K */
    { 3840, 2160, 120, FALSE },
    /* 16:10 resolutions */
    { 1920, 1200,  60, FALSE },     /* WUXGA */
    { 2560, 1600,  60, FALSE },     /* WQXGA */
    /* 4:3 resolutions */
    { 1024,  768,  60, FALSE },     /* XGA */
    { 1600, 1200,  60, FALSE },     /* UXGA */
};

#define PVGPU_NUM_DISPLAY_MODES (sizeof(g_DisplayModes) / sizeof(g_DisplayModes[0]))

/*
 * =============================================================================
 * Heap Allocator (Simple bitmap-based)
 * =============================================================================
 */

typedef struct _PVGPU_HEAP_ALLOCATOR {
    ULONG       BlockSize;          /* Size of each block */
    ULONG       NumBlocks;          /* Total number of blocks */
    ULONG       FreeBlocks;         /* Number of free blocks */
    ULONG       HeapOffset;         /* Offset of heap in shared memory */
    ULONG       HeapSize;           /* Total heap size */
    RTL_BITMAP  Bitmap;             /* Allocation bitmap */
    PULONG      BitmapBuffer;       /* Buffer for bitmap */
    KSPIN_LOCK  Lock;               /* Lock for allocation */
} PVGPU_HEAP_ALLOCATOR, *PPVGPU_HEAP_ALLOCATOR;

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
    
    /* Heap allocator for UMD allocations */
    PVGPU_HEAP_ALLOCATOR        HeapAllocator;
    
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

/* Escape (UMD <-> KMD communication) */
NTSTATUS
PvgpuEscape(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_ESCAPE* Escape
);

/* VidPn Functions (Display Mode Enumeration) */
NTSTATUS
PvgpuIsSupportedVidPn(
    _In_ PVOID MiniportDeviceContext,
    _Inout_ DXGKARG_ISSUPPORTEDVIDPN* IsSupportedVidPn
);

NTSTATUS
PvgpuRecommendFunctionalVidPn(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_RECOMMENDFUNCTIONALVIDPN* RecommendFunctionalVidPn
);

NTSTATUS
PvgpuEnumVidPnCofuncModality(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_ENUMVIDPNCOFUNCMODALITY* EnumCofuncModality
);

NTSTATUS
PvgpuSetVidPnSourceAddress(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_SETVIDPNSOURCEADDRESS* SetVidPnSourceAddress
);

NTSTATUS
PvgpuSetVidPnSourceVisibility(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_SETVIDPNSOURCEVISIBILITY* SetVidPnSourceVisibility
);

NTSTATUS
PvgpuCommitVidPn(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_COMMITVIDPN* CommitVidPn
);

NTSTATUS
PvgpuUpdateActiveVidPnPresentPath(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH* UpdateActiveVidPnPresentPath
);

NTSTATUS
PvgpuRecommendMonitorModes(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_RECOMMENDMONITORMODES* RecommendMonitorModes
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
