/*
 * PVGPU Kernel-Mode Driver
 *
 * WDDM 2.0 display miniport driver for paravirtualized GPU.
 * This driver runs in the Windows guest and communicates with
 * the host backend via shared memory and doorbell registers.
 *
 * SPDX-License-Identifier: MIT
 */

#include "pvgpu.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, PvgpuAddDevice)
#pragma alloc_text(PAGE, PvgpuStartDevice)
#pragma alloc_text(PAGE, PvgpuStopDevice)
#pragma alloc_text(PAGE, PvgpuRemoveDevice)
#endif

/*
 * =============================================================================
 * Global Data
 * =============================================================================
 */

static DRIVER_OBJECT* g_DriverObject = NULL;

/*
 * =============================================================================
 * Driver Entry Point
 * =============================================================================
 */

NTSTATUS
DriverEntry(
    _In_ DRIVER_OBJECT* DriverObject,
    _In_ UNICODE_STRING* RegistryPath
)
{
    NTSTATUS status;
    DRIVER_INITIALIZATION_DATA initData = {0};

    UNREFERENCED_PARAMETER(RegistryPath);

    KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_INFO_LEVEL,
        "PVGPU: DriverEntry - Protocol version %d.%d\n",
        PVGPU_VERSION_MAJOR, PVGPU_VERSION_MINOR));

    g_DriverObject = DriverObject;

    /* Initialize DRIVER_INITIALIZATION_DATA structure */
    initData.Version = DXGKDDI_INTERFACE_VERSION_WDDM2_0;

    /* Required Plug and Play callbacks */
    initData.DxgkDdiAddDevice = PvgpuAddDevice;
    initData.DxgkDdiStartDevice = PvgpuStartDevice;
    initData.DxgkDdiStopDevice = PvgpuStopDevice;
    initData.DxgkDdiRemoveDevice = PvgpuRemoveDevice;

    /* Interrupt handling */
    initData.DxgkDdiInterruptRoutine = PvgpuInterruptRoutine;
    initData.DxgkDdiDpcRoutine = PvgpuDpcRoutine;

    /* Query functions */
    initData.DxgkDdiQueryAdapterInfo = PvgpuQueryAdapterInfo;
    initData.DxgkDdiQueryChildRelations = PvgpuQueryChildRelations;
    initData.DxgkDdiQueryChildStatus = PvgpuQueryChildStatus;
    initData.DxgkDdiQueryDeviceDescriptor = PvgpuQueryDeviceDescriptor;
    initData.DxgkDdiSetPowerState = PvgpuSetPowerState;

    /* Memory management */
    initData.DxgkDdiBuildPagingBuffer = PvgpuBuildPagingBuffer;
    initData.DxgkDdiSubmitCommand = PvgpuSubmitCommand;
    initData.DxgkDdiPreemptCommand = PvgpuPreemptCommand;
    initData.DxgkDdiPatch = PvgpuPatch;

    /* Device/Context management */
    initData.DxgkDdiCreateDevice = PvgpuCreateDevice;
    initData.DxgkDdiDestroyDevice = PvgpuDestroyDevice;
    initData.DxgkDdiCreateContext = PvgpuCreateContext;
    initData.DxgkDdiDestroyContext = PvgpuDestroyContext;

    /* Allocations */
    initData.DxgkDdiCreateAllocation = PvgpuCreateAllocation;
    initData.DxgkDdiDestroyAllocation = PvgpuDestroyAllocation;
    initData.DxgkDdiDescribeAllocation = PvgpuDescribeAllocation;
    initData.DxgkDdiGetStandardAllocationDriverData = PvgpuGetStandardAllocationDriverData;

    /* Present and Render */
    initData.DxgkDdiPresent = PvgpuPresent;
    initData.DxgkDdiRender = PvgpuRender;

    /* Escape for UMD communication */
    initData.DxgkDdiEscape = PvgpuEscape;

    /* VidPn (Display Mode) functions */
    initData.DxgkDdiIsSupportedVidPn = PvgpuIsSupportedVidPn;
    initData.DxgkDdiRecommendFunctionalVidPn = PvgpuRecommendFunctionalVidPn;
    initData.DxgkDdiEnumVidPnCofuncModality = PvgpuEnumVidPnCofuncModality;
    initData.DxgkDdiSetVidPnSourceAddress = PvgpuSetVidPnSourceAddress;
    initData.DxgkDdiSetVidPnSourceVisibility = PvgpuSetVidPnSourceVisibility;
    initData.DxgkDdiCommitVidPn = PvgpuCommitVidPn;
    initData.DxgkDdiUpdateActiveVidPnPresentPath = PvgpuUpdateActiveVidPnPresentPath;
    initData.DxgkDdiRecommendMonitorModes = PvgpuRecommendMonitorModes;

    /* Register with DirectX graphics kernel */
    status = DxgkInitialize(DriverObject, RegistryPath, &initData);

    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL,
            "PVGPU: DxgkInitialize failed with status 0x%08X\n", status));
        return status;
    }

    KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_INFO_LEVEL,
        "PVGPU: DriverEntry completed successfully\n"));

    return STATUS_SUCCESS;
}

/*
 * =============================================================================
 * Device Addition (PnP)
 * =============================================================================
 */

NTSTATUS
PvgpuAddDevice(
    _In_ DEVICE_OBJECT* PhysicalDeviceObject,
    _Outptr_ PVOID* MiniportDeviceContext
)
{
    PPVGPU_DEVICE_CONTEXT context;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);

    KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_INFO_LEVEL,
        "PVGPU: AddDevice\n"));

    /* Allocate device context */
    context = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(PVGPU_DEVICE_CONTEXT), PVGPU_POOL_TAG);
    if (!context) {
        KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL,
            "PVGPU: Failed to allocate device context\n"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(context, sizeof(PVGPU_DEVICE_CONTEXT));
    KeInitializeSpinLock(&context->CommandLock);

    *MiniportDeviceContext = context;

    return STATUS_SUCCESS;
}

/*
 * =============================================================================
 * Device Start
 * =============================================================================
 */

NTSTATUS
PvgpuStartDevice(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGK_START_INFO* DxgkStartInfo,
    _In_ DXGKRNL_INTERFACE* DxgkInterface,
    _Out_ PULONG NumberOfVideoPresentSources,
    _Out_ PULONG NumberOfChildren
)
{
    PPVGPU_DEVICE_CONTEXT context = (PPVGPU_DEVICE_CONTEXT)MiniportDeviceContext;
    NTSTATUS status;
    DXGK_DEVICE_INFO deviceInfo = {0};
    ULONG bar;

    PAGED_CODE();

    KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_INFO_LEVEL,
        "PVGPU: StartDevice\n"));

    /* Save interfaces */
    context->DxgkInterface = *DxgkInterface;
    context->StartInfo = *DxgkStartInfo;
    context->DeviceHandle = DxgkInterface->DeviceHandle;

    /* Get device information */
    status = DxgkInterface->DxgkCbGetDeviceInformation(
        DxgkInterface->DeviceHandle,
        &deviceInfo);

    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL,
            "PVGPU: Failed to get device information: 0x%08X\n", status));
        return status;
    }

    /* Find and map BARs */
    for (bar = 0; bar < deviceInfo.NumberOfBars; bar++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR desc = &deviceInfo.TranslatedResourceList->List[0].PartialResourceList.PartialDescriptors[bar];

        if (desc->Type == CmResourceTypeMemory) {
            /* BAR0 is 4KB (config registers) */
            if (desc->u.Memory.Length == PVGPU_BAR0_SIZE) {
                context->Bar0PhysAddr = desc->u.Memory.Start;
                context->Bar0Length = desc->u.Memory.Length;

                context->Bar0VirtAddr = (volatile ULONG*)MmMapIoSpace(
                    desc->u.Memory.Start,
                    desc->u.Memory.Length,
                    MmNonCached);

                if (!context->Bar0VirtAddr) {
                    KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL,
                        "PVGPU: Failed to map BAR0\n"));
                    return STATUS_INSUFFICIENT_RESOURCES;
                }

                KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_INFO_LEVEL,
                    "PVGPU: BAR0 mapped at %p\n", context->Bar0VirtAddr));
            }
            /* BAR2 is shared memory (256MB+) */
            else if (desc->u.Memory.Length >= PVGPU_DEFAULT_SHMEM_SIZE) {
                context->Bar2PhysAddr = desc->u.Memory.Start;
                context->Bar2Length = desc->u.Memory.Length;

                context->Bar2VirtAddr = (volatile UCHAR*)MmMapIoSpace(
                    desc->u.Memory.Start,
                    desc->u.Memory.Length,
                    MmWriteCombined);

                if (!context->Bar2VirtAddr) {
                    KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL,
                        "PVGPU: Failed to map BAR2\n"));
                    if (context->Bar0VirtAddr) {
                        MmUnmapIoSpace((PVOID)context->Bar0VirtAddr, context->Bar0Length);
                    }
                    return STATUS_INSUFFICIENT_RESOURCES;
                }

                /* Setup pointers into shared memory */
                context->ControlRegion = (volatile PvgpuControlRegion*)context->Bar2VirtAddr;

                KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_INFO_LEVEL,
                    "PVGPU: BAR2 mapped at %p, size %u MB\n",
                    context->Bar2VirtAddr, context->Bar2Length / (1024 * 1024)));
            }
        }
    }

    /* Verify we have both BARs */
    if (!context->Bar0VirtAddr || !context->Bar2VirtAddr) {
        KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL,
            "PVGPU: Required BARs not found\n"));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    /* Verify protocol magic and version */
    if (context->ControlRegion->magic != PVGPU_MAGIC) {
        KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL,
            "PVGPU: Invalid magic: expected 0x%08X, got 0x%08X\n",
            PVGPU_MAGIC, context->ControlRegion->magic));
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    if ((context->ControlRegion->version >> 16) != PVGPU_VERSION_MAJOR) {
        KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL,
            "PVGPU: Protocol version mismatch\n"));
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    /* Setup ring and heap pointers */
    context->CommandRing = context->Bar2VirtAddr + context->ControlRegion->ring_offset;
    context->CommandRingSize = context->ControlRegion->ring_size;
    context->ResourceHeap = context->Bar2VirtAddr + context->ControlRegion->heap_offset;
    context->ResourceHeapSize = context->ControlRegion->heap_size;

    /* Read display configuration */
    context->DisplayWidth = context->ControlRegion->display_width;
    context->DisplayHeight = context->ControlRegion->display_height;
    context->DisplayRefresh = context->ControlRegion->display_refresh;

    KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_INFO_LEVEL,
        "PVGPU: Display %ux%u @ %u Hz\n",
        context->DisplayWidth, context->DisplayHeight, context->DisplayRefresh));

    /* Initialize heap allocator */
    status = PvgpuHeapInit(context);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL,
            "PVGPU: Failed to initialize heap allocator\n"));
        if (context->Bar0VirtAddr) {
            MmUnmapIoSpace((PVOID)context->Bar0VirtAddr, context->Bar0Length);
        }
        if (context->Bar2VirtAddr) {
            MmUnmapIoSpace((PVOID)context->Bar2VirtAddr, context->Bar2Length);
        }
        return status;
    }

    /* We have one video present source and one child (monitor) */
    *NumberOfVideoPresentSources = 1;
    *NumberOfChildren = 1;

    KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_INFO_LEVEL,
        "PVGPU: StartDevice completed successfully\n"));

    return STATUS_SUCCESS;
}

/*
 * =============================================================================
 * Device Stop
 * =============================================================================
 */

NTSTATUS
PvgpuStopDevice(
    _In_ PVOID MiniportDeviceContext
)
{
    PPVGPU_DEVICE_CONTEXT context = (PPVGPU_DEVICE_CONTEXT)MiniportDeviceContext;

    PAGED_CODE();

    KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_INFO_LEVEL,
        "PVGPU: StopDevice\n"));

    /* Destroy heap allocator */
    PvgpuHeapDestroy(context);

    /* Unmap BARs */
    if (context->Bar0VirtAddr) {
        MmUnmapIoSpace((PVOID)context->Bar0VirtAddr, context->Bar0Length);
        context->Bar0VirtAddr = NULL;
    }

    if (context->Bar2VirtAddr) {
        MmUnmapIoSpace((PVOID)context->Bar2VirtAddr, context->Bar2Length);
        context->Bar2VirtAddr = NULL;
        context->ControlRegion = NULL;
        context->CommandRing = NULL;
        context->ResourceHeap = NULL;
    }

    return STATUS_SUCCESS;
}

/*
 * =============================================================================
 * Device Removal
 * =============================================================================
 */

NTSTATUS
PvgpuRemoveDevice(
    _In_ PVOID MiniportDeviceContext
)
{
    PPVGPU_DEVICE_CONTEXT context = (PPVGPU_DEVICE_CONTEXT)MiniportDeviceContext;

    PAGED_CODE();

    KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_INFO_LEVEL,
        "PVGPU: RemoveDevice\n"));

    if (context) {
        ExFreePoolWithTag(context, PVGPU_POOL_TAG);
    }

    return STATUS_SUCCESS;
}

/*
 * =============================================================================
 * Interrupt Handling
 * =============================================================================
 */

BOOLEAN
PvgpuInterruptRoutine(
    _In_ PVOID MiniportDeviceContext,
    _In_ ULONG MessageNumber
)
{
    PPVGPU_DEVICE_CONTEXT context = (PPVGPU_DEVICE_CONTEXT)MiniportDeviceContext;
    ULONG irqStatus;

    UNREFERENCED_PARAMETER(MessageNumber);

    /* Read IRQ status */
    irqStatus = PvgpuReadBar0(context, PVGPU_REG_IRQ_STATUS);

    if (irqStatus == 0) {
        /* Not our interrupt */
        return FALSE;
    }

    /* Acknowledge interrupt */
    PvgpuWriteBar0(context, PVGPU_REG_IRQ_STATUS, irqStatus);

    /* Queue DPC for deferred processing */
    context->DxgkInterface.DxgkCbQueueDpc(context->DeviceHandle);

    return TRUE;
}

VOID
PvgpuDpcRoutine(
    _In_ PVOID MiniportDeviceContext
)
{
    PPVGPU_DEVICE_CONTEXT context = (PPVGPU_DEVICE_CONTEXT)MiniportDeviceContext;
    ULONG64 completedFence;

    /* Check for completed fences */
    completedFence = context->ControlRegion->host_fence_completed;

    if (completedFence > 0) {
        /* Notify DXG kernel of fence completion */
        DXGKARGCB_NOTIFY_INTERRUPT_DATA notifyData = {0};
        notifyData.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
        notifyData.DmaCompleted.SubmissionFenceId = (UINT)completedFence;
        notifyData.DmaCompleted.NodeOrdinal = 0;
        notifyData.DmaCompleted.EngineOrdinal = 0;

        context->DxgkInterface.DxgkCbNotifyInterrupt(
            context->DeviceHandle,
            &notifyData);

        context->DxgkInterface.DxgkCbNotifyDpc(context->DeviceHandle);
    }
}

/*
 * =============================================================================
 * Query Adapter Info (Stub)
 * =============================================================================
 */

NTSTATUS
PvgpuQueryAdapterInfo(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_QUERYADAPTERINFO* QueryAdapterInfo
)
{
    PPVGPU_DEVICE_CONTEXT context = (PPVGPU_DEVICE_CONTEXT)MiniportDeviceContext;

    switch (QueryAdapterInfo->Type) {
    case DXGKQAITYPE_DRIVERCAPS:
    {
        DXGK_DRIVERCAPS* caps = (DXGK_DRIVERCAPS*)QueryAdapterInfo->pOutputData;
        RtlZeroMemory(caps, sizeof(DXGK_DRIVERCAPS));

        /* Basic driver capabilities */
        caps->HighestAcceptableAddress.QuadPart = 0xFFFFFFFFFFFFFFFF;
        caps->MaxAllocationListSlotId = 256;
        caps->ApertureSegmentCommitLimit = 0;
        caps->MaxPointerWidth = 64;
        caps->MaxPointerHeight = 64;
        caps->PointerCaps.Color = TRUE;
        caps->PointerCaps.MaskedColor = TRUE;

        /* Scheduling capabilities */
        caps->SchedulingCaps.MultiEngineAware = FALSE;
        caps->SchedulingCaps.VSyncPowerSaveAware = TRUE;

        /* Memory management */
        caps->MemoryManagementCaps.OutOfOrderLock = TRUE;
        caps->MemoryManagementCaps.PagingNode = 0;

        /* GPU engine count */
        caps->GpuEngineTopology.NbAsymetricProcessingNodes = 1;

        /* Indicate we support WDDM 2.0 */
        caps->WDDMVersion = DXGKDDI_WDDMv2;

        return STATUS_SUCCESS;
    }

    case DXGKQAITYPE_QUERYSEGMENT:
    {
        DXGK_QUERYSEGMENTOUT* segmentInfo = (DXGK_QUERYSEGMENTOUT*)QueryAdapterInfo->pOutputData;

        if (QueryAdapterInfo->OutputDataSize < sizeof(DXGK_QUERYSEGMENTOUT)) {
            return STATUS_BUFFER_TOO_SMALL;
        }

        /* We have one segment: the shared memory */
        segmentInfo->NbSegment = 1;

        if (segmentInfo->pSegmentDescriptor) {
            DXGK_SEGMENTDESCRIPTOR* seg = &segmentInfo->pSegmentDescriptor[0];
            RtlZeroMemory(seg, sizeof(DXGK_SEGMENTDESCRIPTOR));

            seg->BaseAddress.QuadPart = 0;  /* Aperture segment */
            seg->CpuTranslatedAddress.QuadPart = context->Bar2PhysAddr.QuadPart;
            seg->Size = context->ResourceHeapSize;
            seg->NbOfBanks = 0;
            seg->pBankRangeTable = NULL;
            seg->CommitLimit = context->ResourceHeapSize;
            seg->Flags.Aperture = TRUE;
            seg->Flags.CpuVisible = TRUE;
        }

        return STATUS_SUCCESS;
    }

    default:
        return STATUS_NOT_SUPPORTED;
    }
}

/*
 * =============================================================================
 * Child Enumeration (Stub)
 * =============================================================================
 */

NTSTATUS
PvgpuQueryChildRelations(
    _In_ PVOID MiniportDeviceContext,
    _Inout_ DXGK_CHILD_DESCRIPTOR* ChildRelations,
    _In_ ULONG ChildRelationsSize
)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(ChildRelationsSize);

    /* We have one child: a video output */
    ChildRelations[0].ChildDeviceType = TypeVideoOutput;
    ChildRelations[0].ChildCapabilities.HpdAwareness = HpdAwarenessAlwaysConnected;
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = D3DKMDT_VOT_INTERNAL;
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
    ChildRelations[0].AcpiUid = 0;
    ChildRelations[0].ChildUid = 1;

    /* Mark end of list */
    ChildRelations[1].ChildDeviceType = TypeUninitialized;

    return STATUS_SUCCESS;
}

NTSTATUS
PvgpuQueryChildStatus(
    _In_ PVOID MiniportDeviceContext,
    _Inout_ DXGK_CHILD_STATUS* ChildStatus,
    _In_ BOOLEAN NonDestructiveOnly
)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(NonDestructiveOnly);

    if (ChildStatus->ChildUid == 1) {
        if (ChildStatus->Type == StatusConnection) {
            ChildStatus->HotPlug.Connected = TRUE;
        }
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_PARAMETER;
}

NTSTATUS
PvgpuQueryDeviceDescriptor(
    _In_ PVOID MiniportDeviceContext,
    _In_ ULONG ChildUid,
    _Inout_ DXGK_DEVICE_DESCRIPTOR* DeviceDescriptor
)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(ChildUid);

    /* Return no EDID - we'll use a hardcoded mode */
    DeviceDescriptor->DescriptorLength = 0;

    return STATUS_MONITOR_NO_DESCRIPTOR;
}

/*
 * =============================================================================
 * Power Management (Stub)
 * =============================================================================
 */

NTSTATUS
PvgpuSetPowerState(
    _In_ PVOID MiniportDeviceContext,
    _In_ ULONG HardwareUid,
    _In_ DEVICE_POWER_STATE DevicePowerState,
    _In_ POWER_ACTION ActionType
)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(HardwareUid);
    UNREFERENCED_PARAMETER(DevicePowerState);
    UNREFERENCED_PARAMETER(ActionType);

    return STATUS_SUCCESS;
}

/*
 * =============================================================================
 * Memory Management (Stubs)
 * =============================================================================
 */

NTSTATUS
PvgpuBuildPagingBuffer(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_BUILDPAGINGBUFFER* BuildPagingBuffer
)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);

    /*
     * Handle paging operations for memory transfers.
     * In our paravirtualized model, actual data movement happens on the host
     * side via shared memory - we just need to produce valid DMA buffer entries
     * so the scheduler is satisfied.
     */

    switch (BuildPagingBuffer->Operation) {
    case DXGK_OPERATION_TRANSFER:
        /*
         * Transfer between memory segments. For PVGPU, resources live in
         * shared memory accessible to both guest and host, so we don't need
         * actual GPU DMA. Just advance the DMA buffer pointer to indicate
         * we "handled" it.
         */
        {
            ULONG transferSize = sizeof(ULONG) * 2;  /* opcode + size marker */
            if ((UCHAR*)BuildPagingBuffer->pDmaBuffer + transferSize >
                (UCHAR*)BuildPagingBuffer->pDmaBufferEnd) {
                return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
            }
            /* Write a NOP-like transfer marker */
            *(ULONG*)BuildPagingBuffer->pDmaBuffer = 0;  /* NOP opcode */
            BuildPagingBuffer->pDmaBuffer =
                (UCHAR*)BuildPagingBuffer->pDmaBuffer + transferSize;
        }
        break;

    case DXGK_OPERATION_FILL:
        /*
         * Fill a memory region with a pattern. Used for clearing allocations.
         * We can handle this via shared memory on the host.
         */
        {
            ULONG fillSize = sizeof(ULONG) * 2;
            if ((UCHAR*)BuildPagingBuffer->pDmaBuffer + fillSize >
                (UCHAR*)BuildPagingBuffer->pDmaBufferEnd) {
                return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
            }
            *(ULONG*)BuildPagingBuffer->pDmaBuffer = 0;
            BuildPagingBuffer->pDmaBuffer =
                (UCHAR*)BuildPagingBuffer->pDmaBuffer + fillSize;
        }
        break;

    case DXGK_OPERATION_DISCARD_CONTENT:
        /* Content can be discarded - nothing to do */
        BuildPagingBuffer->pDmaBuffer =
            (UCHAR*)BuildPagingBuffer->pDmaBuffer + sizeof(ULONG);
        break;

    default:
        /* Other operations: just advance DMA buffer minimally */
        BuildPagingBuffer->pDmaBuffer =
            (UCHAR*)BuildPagingBuffer->pDmaBuffer + sizeof(ULONG);
        break;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
PvgpuSubmitCommand(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_SUBMITCOMMAND* SubmitCommand
)
{
    PPVGPU_DEVICE_CONTEXT context = (PPVGPU_DEVICE_CONTEXT)MiniportDeviceContext;

    UNREFERENCED_PARAMETER(SubmitCommand);

    /* Ring the doorbell to notify host */
    PvgpuRingDoorbell(context);

    return STATUS_SUCCESS;
}

NTSTATUS
PvgpuPreemptCommand(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_PREEMPTCOMMAND* PreemptCommand
)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(PreemptCommand);

    /* Preemption not supported yet */
    return STATUS_SUCCESS;
}

NTSTATUS
PvgpuPatch(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_PATCH* Patch
)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(Patch);

    return STATUS_SUCCESS;
}

/*
 * =============================================================================
 * Device/Context Management (Stubs)
 * =============================================================================
 */

NTSTATUS
PvgpuCreateDevice(
    _In_ PVOID MiniportDeviceContext,
    _Inout_ DXGKARG_CREATEDEVICE* CreateDevice
)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);

    /* Allocate a device context if needed */
    CreateDevice->hDevice = NULL;  /* Use default */

    return STATUS_SUCCESS;
}

NTSTATUS
PvgpuDestroyDevice(
    _In_ PVOID MiniportDeviceContext,
    _In_ HANDLE DeviceHandle
)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(DeviceHandle);

    return STATUS_SUCCESS;
}

NTSTATUS
PvgpuCreateContext(
    _In_ PVOID MiniportDeviceContext,
    _Inout_ DXGKARG_CREATECONTEXT* CreateContext
)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(CreateContext);

    return STATUS_SUCCESS;
}

NTSTATUS
PvgpuDestroyContext(
    _In_ PVOID MiniportDeviceContext,
    _In_ HANDLE ContextHandle
)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(ContextHandle);

    return STATUS_SUCCESS;
}

/*
 * =============================================================================
 * Allocations (Stubs)
 * =============================================================================
 */

NTSTATUS
PvgpuCreateAllocation(
    _In_ PVOID MiniportDeviceContext,
    _Inout_ DXGKARG_CREATEALLOCATION* CreateAllocation
)
{
    PPVGPU_DEVICE_CONTEXT context = (PPVGPU_DEVICE_CONTEXT)MiniportDeviceContext;
    ULONG i;

    /*
     * Process each allocation request. For PVGPU, allocations map to regions
     * in the shared memory heap that both guest and host can access.
     * The actual GPU resource creation happens on the host side.
     */
    for (i = 0; i < CreateAllocation->NumAllocations; i++) {
        DXGK_ALLOCATIONINFO* allocInfo = &CreateAllocation->pAllocationInfo[i];
        ULONG allocationSize;

        /*
         * Determine allocation size from private driver data.
         * If private data is provided, use it; otherwise use a default page.
         */
        if (allocInfo->pPrivateDriverData != NULL &&
            allocInfo->PrivateDriverDataSize >= sizeof(ULONG)) {
            allocationSize = *(ULONG*)allocInfo->pPrivateDriverData;
        } else {
            allocationSize = PVGPU_HEAP_BLOCK_SIZE;  /* Default 4KB */
        }

        /* Ensure minimum allocation size */
        if (allocationSize == 0) {
            allocationSize = PVGPU_HEAP_BLOCK_SIZE;
        }

        /* Align to heap block size */
        allocationSize = (allocationSize + PVGPU_HEAP_BLOCK_SIZE - 1) &
                         ~(PVGPU_HEAP_BLOCK_SIZE - 1);

        /*
         * Set allocation properties:
         * - Segment 1 is our shared memory segment (BAR2)
         * - CpuVisible so UMD can map and write commands
         */
        allocInfo->Alignment = PVGPU_HEAP_BLOCK_SIZE;
        allocInfo->Size = allocationSize;
        allocInfo->PitchAlignedSize = 0;
        allocInfo->HintedBank.Value = 0;
        allocInfo->PreferredSegment.Value = 0;
        allocInfo->SupportedReadSegmentSet = 1;   /* Segment 1 */
        allocInfo->SupportedWriteSegmentSet = 1;  /* Segment 1 */
        allocInfo->EvictionSegmentSet = 0;
        allocInfo->MaximumRenamingListLength = 0;
        allocInfo->pAllocationUsageHint = NULL;
        allocInfo->Flags.Value = 0;
        allocInfo->Flags.CpuVisible = 1;
    }

    UNREFERENCED_PARAMETER(context);

    return STATUS_SUCCESS;
}

NTSTATUS
PvgpuDestroyAllocation(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_DESTROYALLOCATION* DestroyAllocation
)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(DestroyAllocation);

    return STATUS_SUCCESS;
}

NTSTATUS
PvgpuDescribeAllocation(
    _In_ PVOID MiniportDeviceContext,
    _Inout_ DXGKARG_DESCRIBEALLOCATION* DescribeAllocation
)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(DescribeAllocation);

    return STATUS_SUCCESS;
}

NTSTATUS
PvgpuGetStandardAllocationDriverData(
    _In_ PVOID MiniportDeviceContext,
    _Inout_ DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA* StandardAllocation
)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(StandardAllocation);

    return STATUS_NOT_SUPPORTED;
}

/*
 * =============================================================================
 * Present and Render (Stubs)
 * =============================================================================
 */

NTSTATUS
PvgpuPresent(
    _In_ PVOID MiniportDeviceContext,
    _Inout_ DXGKARG_PRESENT* Present
)
{
    PPVGPU_DEVICE_CONTEXT context = (PPVGPU_DEVICE_CONTEXT)MiniportDeviceContext;
    PvgpuCmdPresent cmd;
    NTSTATUS status;

    /*
     * Build a present command and submit it to the ring buffer.
     * The host backend will pick this up, do the actual present/flip
     * via its D3D11 swapchain, and signal completion.
     */
    RtlZeroMemory(&cmd, sizeof(cmd));
    cmd.header.type = PVGPU_CMD_PRESENT;
    cmd.header.size = sizeof(PvgpuCmdPresent);
    cmd.header.fence_id = 0;  /* Fence managed by SubmitCommand */

    /*
     * Use the source allocation handle as the backbuffer ID.
     * The host will use this to identify which resource to present.
     */
    if (Present->pSource != NULL) {
        cmd.backbuffer_id = (UINT32)(ULONG_PTR)Present->hAllocation;
    }
    cmd.sync_interval = 1;  /* Default VSync on */
    cmd.flags = 0;

    /* Write present command to DMA buffer */
    if ((UCHAR*)Present->pDmaBuffer + sizeof(cmd) <=
        (UCHAR*)Present->pDmaBufferEnd) {
        RtlCopyMemory(Present->pDmaBuffer, &cmd, sizeof(cmd));
        Present->pDmaBuffer = (UCHAR*)Present->pDmaBuffer + sizeof(cmd);
    }

    /* Submit to ring buffer and notify host */
    status = PvgpuSubmitToRing(context, &cmd, sizeof(cmd));
    if (NT_SUCCESS(status)) {
        PvgpuRingDoorbell(context);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
PvgpuRender(
    _In_ PVOID MiniportDeviceContext,
    _Inout_ DXGKARG_RENDER* Render
)
{
    PPVGPU_DEVICE_CONTEXT context = (PPVGPU_DEVICE_CONTEXT)MiniportDeviceContext;
    SIZE_T commandSize;
    NTSTATUS status;

    /*
     * The UMD builds a command buffer in user-mode memory. The KMD's job in
     * Render() is to validate and copy those commands into the DMA buffer
     * for later submission by SubmitCommand().
     *
     * For PVGPU, UMD commands are already in our protocol format, so we
     * can copy them directly. In a real driver, we'd do validation and
     * potentially translate addresses.
     */

    /* Calculate how many bytes the UMD submitted */
    commandSize = (UCHAR*)Render->pCommand + Render->CommandLength -
                  (UCHAR*)Render->pCommand;
    if (commandSize == 0) {
        /* Nothing to render */
        return STATUS_SUCCESS;
    }

    /* Verify DMA buffer has enough space */
    if ((UCHAR*)Render->pDmaBuffer + commandSize >
        (UCHAR*)Render->pDmaBufferEnd) {
        /* Request a larger DMA buffer */
        Render->MultipassOffset = 0;
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    }

    /*
     * Copy UMD commands directly to DMA buffer.
     * The UMD command format matches our ring buffer protocol, so
     * no translation is needed. The commands will be submitted to
     * the ring buffer in SubmitCommand().
     */
    __try {
        RtlCopyMemory(Render->pDmaBuffer, Render->pCommand, commandSize);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return STATUS_INVALID_USER_BUFFER;
    }

    /* Advance DMA buffer pointer past what we wrote */
    Render->pDmaBuffer = (UCHAR*)Render->pDmaBuffer + commandSize;

    /* Also submit directly to ring buffer for host processing */
    __try {
        status = PvgpuSubmitToRing(context, Render->pCommand, (ULONG)commandSize);
        if (NT_SUCCESS(status)) {
            PvgpuRingDoorbell(context);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        /* Continue even if ring submission fails - DMA buffer is valid */
    }

    /* Set private driver data size (used by SubmitCommand) */
    if (Render->pDmaBufferPrivateData != NULL &&
        Render->pDmaBufferPrivateDataSize >= sizeof(ULONG)) {
        *(ULONG*)Render->pDmaBufferPrivateData = (ULONG)commandSize;
        Render->pDmaBufferPrivateData =
            (UCHAR*)Render->pDmaBufferPrivateData + sizeof(ULONG);
    }

    return STATUS_SUCCESS;
}

/*
 * =============================================================================
 * Ring Buffer Submission
 * =============================================================================
 */

NTSTATUS
PvgpuSubmitToRing(
    _In_ PPVGPU_DEVICE_CONTEXT Context,
    _In_ PVOID CommandData,
    _In_ ULONG CommandSize
)
{
    KIRQL oldIrql;
    ULONG64 producer, consumer;
    ULONG64 writeOffset;
    ULONG available;

    KeAcquireSpinLock(&Context->CommandLock, &oldIrql);

    /* Read current positions */
    producer = Context->ControlRegion->producer_ptr;
    consumer = Context->ControlRegion->consumer_ptr;

    /* Calculate available space */
    available = Context->CommandRingSize - (ULONG)(producer - consumer);

    if (available < CommandSize) {
        KeReleaseSpinLock(&Context->CommandLock, oldIrql);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Write command to ring (handle wrap-around) */
    writeOffset = producer % Context->CommandRingSize;

    if (writeOffset + CommandSize <= Context->CommandRingSize) {
        /* No wrap needed */
        RtlCopyMemory((PVOID)(Context->CommandRing + writeOffset), CommandData, CommandSize);
    } else {
        /* Wrap around */
        ULONG firstPart = Context->CommandRingSize - (ULONG)writeOffset;
        RtlCopyMemory((PVOID)(Context->CommandRing + writeOffset), CommandData, firstPart);
        RtlCopyMemory((PVOID)Context->CommandRing, (UCHAR*)CommandData + firstPart, CommandSize - firstPart);
    }

    /* Memory barrier to ensure writes are visible */
    KeMemoryBarrier();

    /* Update producer pointer */
    Context->ControlRegion->producer_ptr = producer + CommandSize;

    KeReleaseSpinLock(&Context->CommandLock, oldIrql);

    return STATUS_SUCCESS;
}

/*
 * =============================================================================
 * Heap Allocator Implementation
 * =============================================================================
 */

NTSTATUS
PvgpuHeapInit(
    _In_ PPVGPU_DEVICE_CONTEXT Context
)
{
    PPVGPU_HEAP_ALLOCATOR alloc = &Context->HeapAllocator;
    ULONG bitmapSizeBytes;

    /* Calculate block count */
    alloc->HeapOffset = Context->ControlRegion->heap_offset;
    alloc->HeapSize = Context->ResourceHeapSize;
    alloc->BlockSize = PVGPU_HEAP_BLOCK_SIZE;
    alloc->NumBlocks = alloc->HeapSize / alloc->BlockSize;
    alloc->FreeBlocks = alloc->NumBlocks;

    if (alloc->NumBlocks > PVGPU_HEAP_MAX_BLOCKS) {
        alloc->NumBlocks = PVGPU_HEAP_MAX_BLOCKS;
    }

    /* Allocate bitmap buffer */
    bitmapSizeBytes = (alloc->NumBlocks + 31) / 32 * sizeof(ULONG);
    alloc->BitmapBuffer = ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        bitmapSizeBytes,
        PVGPU_POOL_TAG);

    if (!alloc->BitmapBuffer) {
        KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL,
            "PVGPU: Failed to allocate heap bitmap\n"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Initialize bitmap (all blocks free = 0) */
    RtlZeroMemory(alloc->BitmapBuffer, bitmapSizeBytes);
    RtlInitializeBitMap(&alloc->Bitmap, alloc->BitmapBuffer, alloc->NumBlocks);

    KeInitializeSpinLock(&alloc->Lock);

    KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_INFO_LEVEL,
        "PVGPU: Heap initialized: %u blocks of %u bytes\n",
        alloc->NumBlocks, alloc->BlockSize));

    return STATUS_SUCCESS;
}

VOID
PvgpuHeapDestroy(
    _In_ PPVGPU_DEVICE_CONTEXT Context
)
{
    PPVGPU_HEAP_ALLOCATOR alloc = &Context->HeapAllocator;

    if (alloc->BitmapBuffer) {
        ExFreePoolWithTag(alloc->BitmapBuffer, PVGPU_POOL_TAG);
        alloc->BitmapBuffer = NULL;
    }
}

NTSTATUS
PvgpuHeapAlloc(
    _In_ PPVGPU_DEVICE_CONTEXT Context,
    _In_ ULONG Size,
    _In_ ULONG Alignment,
    _Out_ PULONG Offset,
    _Out_ PULONG AllocatedSize
)
{
    PPVGPU_HEAP_ALLOCATOR alloc = &Context->HeapAllocator;
    KIRQL oldIrql;
    ULONG blocksNeeded;
    ULONG startBlock;
    ULONG alignmentBlocks;

    *Offset = 0;
    *AllocatedSize = 0;

    /* Calculate blocks needed (round up) */
    blocksNeeded = (Size + alloc->BlockSize - 1) / alloc->BlockSize;

    /* Handle alignment */
    if (Alignment > alloc->BlockSize) {
        alignmentBlocks = Alignment / alloc->BlockSize;
    } else {
        alignmentBlocks = 1;
    }

    KeAcquireSpinLock(&alloc->Lock, &oldIrql);

    /* Find contiguous free blocks */
    startBlock = RtlFindClearBits(&alloc->Bitmap, blocksNeeded, 0);

    if (startBlock == 0xFFFFFFFF) {
        KeReleaseSpinLock(&alloc->Lock, oldIrql);
        KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_WARNING_LEVEL,
            "PVGPU: Heap allocation failed: no space for %u blocks\n", blocksNeeded));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Align start block if needed */
    if (alignmentBlocks > 1) {
        ULONG alignedStart = (startBlock + alignmentBlocks - 1) & ~(alignmentBlocks - 1);
        if (alignedStart != startBlock) {
            /* Try to find from aligned position */
            startBlock = RtlFindClearBits(&alloc->Bitmap, blocksNeeded, alignedStart);
            if (startBlock == 0xFFFFFFFF) {
                KeReleaseSpinLock(&alloc->Lock, oldIrql);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
        }
    }

    /* Mark blocks as allocated */
    RtlSetBits(&alloc->Bitmap, startBlock, blocksNeeded);
    alloc->FreeBlocks -= blocksNeeded;

    KeReleaseSpinLock(&alloc->Lock, oldIrql);

    *Offset = alloc->HeapOffset + (startBlock * alloc->BlockSize);
    *AllocatedSize = blocksNeeded * alloc->BlockSize;

    KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_TRACE_LEVEL,
        "PVGPU: Heap alloc: offset=%u size=%u\n", *Offset, *AllocatedSize));

    return STATUS_SUCCESS;
}

NTSTATUS
PvgpuHeapFree(
    _In_ PPVGPU_DEVICE_CONTEXT Context,
    _In_ ULONG Offset,
    _In_ ULONG Size
)
{
    PPVGPU_HEAP_ALLOCATOR alloc = &Context->HeapAllocator;
    KIRQL oldIrql;
    ULONG startBlock;
    ULONG numBlocks;

    /* Validate offset is within heap */
    if (Offset < alloc->HeapOffset || Offset >= alloc->HeapOffset + alloc->HeapSize) {
        return STATUS_INVALID_PARAMETER;
    }

    startBlock = (Offset - alloc->HeapOffset) / alloc->BlockSize;
    numBlocks = (Size + alloc->BlockSize - 1) / alloc->BlockSize;

    if (startBlock + numBlocks > alloc->NumBlocks) {
        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireSpinLock(&alloc->Lock, &oldIrql);

    /* Mark blocks as free */
    RtlClearBits(&alloc->Bitmap, startBlock, numBlocks);
    alloc->FreeBlocks += numBlocks;

    KeReleaseSpinLock(&alloc->Lock, oldIrql);

    KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_TRACE_LEVEL,
        "PVGPU: Heap free: offset=%u size=%u\n", Offset, Size));

    return STATUS_SUCCESS;
}

/*
 * =============================================================================
 * Escape Handler (UMD <-> KMD Communication)
 * =============================================================================
 */

NTSTATUS
PvgpuEscape(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_ESCAPE* Escape
)
{
    PPVGPU_DEVICE_CONTEXT context = (PPVGPU_DEVICE_CONTEXT)MiniportDeviceContext;
    PvgpuEscapeHeader* header;
    NTSTATUS status = STATUS_SUCCESS;

    if (!Escape->pPrivateDriverData || Escape->PrivateDriverDataSize < sizeof(PvgpuEscapeHeader)) {
        return STATUS_INVALID_PARAMETER;
    }

    header = (PvgpuEscapeHeader*)Escape->pPrivateDriverData;

    switch (header->escape_code) {
    case PVGPU_ESCAPE_GET_SHMEM_INFO:
    {
        PvgpuEscapeGetShmemInfo* info = (PvgpuEscapeGetShmemInfo*)header;

        if (Escape->PrivateDriverDataSize < sizeof(PvgpuEscapeGetShmemInfo)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        info->shmem_base = (ULONG64)(ULONG_PTR)context->Bar2VirtAddr;
        info->shmem_size = context->Bar2Length;
        info->ring_offset = context->ControlRegion->ring_offset;
        info->ring_size = context->CommandRingSize;
        info->heap_offset = context->ControlRegion->heap_offset;
        info->heap_size = context->ResourceHeapSize;
        info->features = context->ControlRegion->features;
        info->header.status = PVGPU_ERROR_SUCCESS;

        KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_TRACE_LEVEL,
            "PVGPU: Escape GET_SHMEM_INFO: base=%p size=%u\n",
            (PVOID)info->shmem_base, info->shmem_size));
        break;
    }

    case PVGPU_ESCAPE_ALLOC_HEAP:
    {
        PvgpuEscapeAllocHeap* alloc = (PvgpuEscapeAllocHeap*)header;

        if (Escape->PrivateDriverDataSize < sizeof(PvgpuEscapeAllocHeap)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        status = PvgpuHeapAlloc(
            context,
            alloc->size,
            alloc->alignment,
            &alloc->offset,
            &alloc->allocated_size);

        alloc->header.status = NT_SUCCESS(status) ? 
            PVGPU_ERROR_SUCCESS : PVGPU_ERROR_OUT_OF_MEMORY;
        break;
    }

    case PVGPU_ESCAPE_FREE_HEAP:
    {
        PvgpuEscapeFreeHeap* free = (PvgpuEscapeFreeHeap*)header;

        if (Escape->PrivateDriverDataSize < sizeof(PvgpuEscapeFreeHeap)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        status = PvgpuHeapFree(context, free->offset, free->size);
        free->header.status = NT_SUCCESS(status) ? 
            PVGPU_ERROR_SUCCESS : PVGPU_ERROR_INVALID_PARAMETER;
        break;
    }

    case PVGPU_ESCAPE_SUBMIT_COMMANDS:
    {
        PvgpuEscapeSubmitCommands* submit = (PvgpuEscapeSubmitCommands*)header;
        PVOID cmdData;

        if (Escape->PrivateDriverDataSize < sizeof(PvgpuEscapeSubmitCommands)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        /* Get command data from heap */
        cmdData = (PVOID)(context->Bar2VirtAddr + submit->command_offset);

        /* Submit to ring */
        status = PvgpuSubmitToRing(context, cmdData, submit->command_size);

        if (NT_SUCCESS(status)) {
            /* Ring doorbell */
            PvgpuRingDoorbell(context);
            submit->producer_ptr = context->ControlRegion->producer_ptr;
            submit->header.status = PVGPU_ERROR_SUCCESS;
        } else {
            submit->header.status = PVGPU_ERROR_RING_FULL;
        }
        break;
    }

    case PVGPU_ESCAPE_WAIT_FENCE:
    {
        PvgpuEscapeWaitFence* wait = (PvgpuEscapeWaitFence*)header;
        LARGE_INTEGER timeout;
        ULONG64 completedFence;
        ULONG32 deviceStatus;

        if (Escape->PrivateDriverDataSize < sizeof(PvgpuEscapeWaitFence)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        /* Calculate timeout */
        if (wait->timeout_ms == 0) {
            timeout.QuadPart = MAXLONGLONG; /* Infinite */
        } else {
            timeout.QuadPart = -((LONGLONG)wait->timeout_ms * 10000); /* Relative, 100ns units */
        }

        /* Poll for fence completion (simple implementation) */
        /* In production, this should use an event/interrupt */
        while (TRUE) {
            /* Check device status - abort if backend disconnected or error */
            deviceStatus = context->ControlRegion->status;
            if (deviceStatus & PVGPU_STATUS_SHUTDOWN) {
                KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_WARNING_LEVEL,
                    "PVGPU: WaitFence - Backend shutdown detected\n"));
                status = STATUS_DEVICE_REMOVED;
                wait->header.status = PVGPU_ERROR_BACKEND_DISCONNECTED;
                break;
            }
            if (deviceStatus & PVGPU_STATUS_DEVICE_LOST) {
                KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_WARNING_LEVEL,
                    "PVGPU: WaitFence - Device lost detected\n"));
                status = STATUS_DEVICE_REMOVED;
                wait->header.status = PVGPU_ERROR_DEVICE_LOST;
                break;
            }
            if ((deviceStatus & PVGPU_STATUS_ERROR) && 
                context->ControlRegion->error_code != PVGPU_ERROR_SUCCESS) {
                KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_WARNING_LEVEL,
                    "PVGPU: WaitFence - Backend error %u\n", 
                    context->ControlRegion->error_code));
                /* Non-fatal error - continue but report it */
            }

            completedFence = context->ControlRegion->host_fence_completed;
            if (completedFence >= wait->fence_value) {
                break;
            }

            /* Yield and check for timeout */
            LARGE_INTEGER delay;
            delay.QuadPart = -10000; /* 1ms */
            KeDelayExecutionThread(KernelMode, FALSE, &delay);

            /* Simple timeout check */
            if (wait->timeout_ms != 0) {
                wait->timeout_ms--;
                if (wait->timeout_ms == 0) {
                    status = STATUS_TIMEOUT;
                    break;
                }
            }
        }

        if (NT_SUCCESS(status)) {
            wait->completed_fence = context->ControlRegion->host_fence_completed;
            wait->header.status = PVGPU_ERROR_SUCCESS;
        } else if (status == STATUS_TIMEOUT) {
            wait->completed_fence = context->ControlRegion->host_fence_completed;
            wait->header.status = PVGPU_ERROR_TIMEOUT;
        }
        /* Other error statuses already have header.status set above */
        break;
    }

    case PVGPU_ESCAPE_GET_CAPS:
    {
        PvgpuEscapeGetCaps* caps = (PvgpuEscapeGetCaps*)header;

        if (Escape->PrivateDriverDataSize < sizeof(PvgpuEscapeGetCaps)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        caps->features = context->ControlRegion->features;
        caps->max_texture_size = 16384;      /* D3D11 max */
        caps->max_render_targets = 8;        /* D3D11 standard */
        caps->max_vertex_streams = 16;
        caps->max_constant_buffers = 14;     /* Per stage */
        caps->display_width = context->DisplayWidth;
        caps->display_height = context->DisplayHeight;
        caps->display_refresh = context->DisplayRefresh;
        caps->header.status = PVGPU_ERROR_SUCCESS;
        break;
    }

    case PVGPU_ESCAPE_RING_DOORBELL:
    {
        PvgpuRingDoorbell(context);
        header->status = PVGPU_ERROR_SUCCESS;
        break;
    }

    case PVGPU_ESCAPE_SET_DISPLAY_MODE:
    {
        PvgpuEscapeSetDisplayMode* mode = (PvgpuEscapeSetDisplayMode*)header;

        if (Escape->PrivateDriverDataSize < sizeof(PvgpuEscapeSetDisplayMode)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        /* Validate mode dimensions */
        if (mode->width == 0 || mode->height == 0 || mode->refresh_rate == 0) {
            mode->header.status = PVGPU_ERROR_INVALID_PARAMETER;
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        /* Update local display state */
        context->DisplayWidth = mode->width;
        context->DisplayHeight = mode->height;
        context->DisplayRefresh = mode->refresh_rate;

        /* Update control region to notify backend */
        if (context->ControlRegion) {
            context->ControlRegion->display_width = mode->width;
            context->ControlRegion->display_height = mode->height;
            context->ControlRegion->display_refresh = mode->refresh_rate;
        }

        KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_INFO_LEVEL,
            "PVGPU: SetDisplayMode: %ux%u @ %u Hz\n",
            mode->width, mode->height, mode->refresh_rate));

        mode->header.status = PVGPU_ERROR_SUCCESS;
        break;
    }

    default:
        KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_WARNING_LEVEL,
            "PVGPU: Unknown escape code: 0x%08X\n", header->escape_code));
        header->status = PVGPU_ERROR_INVALID_COMMAND;
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    return status;
}

/*
 * =============================================================================
 * VidPn Functions (Display Mode Enumeration)
 * =============================================================================
 */

/*
 * Helper: Add a source mode to the pinned source mode set
 */
static NTSTATUS
PvgpuAddSourceMode(
    _In_ D3DKMDT_HVIDPNSOURCEMODESET hModeSet,
    _In_ CONST DXGK_VIDPNSOURCEMODESET_INTERFACE* pModeSetInterface,
    _In_ ULONG Width,
    _In_ ULONG Height
)
{
    NTSTATUS status;
    D3DKMDT_VIDPN_SOURCE_MODE* pMode = NULL;

    status = pModeSetInterface->pfnCreateNewModeInfo(hModeSet, &pMode);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    pMode->Type = D3DKMDT_RMT_GRAPHICS;
    pMode->Format.Graphics.PrimSurfSize.cx = Width;
    pMode->Format.Graphics.PrimSurfSize.cy = Height;
    pMode->Format.Graphics.VisibleRegionSize.cx = Width;
    pMode->Format.Graphics.VisibleRegionSize.cy = Height;
    pMode->Format.Graphics.Stride = Width * 4;  /* BGRA */
    pMode->Format.Graphics.PixelFormat = D3DDDIFMT_A8R8G8B8;
    pMode->Format.Graphics.ColorBasis = D3DKMDT_CB_SRGB;
    pMode->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;

    status = pModeSetInterface->pfnAddMode(hModeSet, pMode);
    if (!NT_SUCCESS(status)) {
        pModeSetInterface->pfnReleaseModeInfo(hModeSet, pMode);
    }

    return status;
}

/*
 * Helper: Add a target mode to the pinned target mode set
 */
static NTSTATUS
PvgpuAddTargetMode(
    _In_ D3DKMDT_HVIDPNTARGETMODESET hModeSet,
    _In_ CONST DXGK_VIDPNTARGETMODESET_INTERFACE* pModeSetInterface,
    _In_ ULONG Width,
    _In_ ULONG Height,
    _In_ ULONG RefreshRate
)
{
    NTSTATUS status;
    D3DKMDT_VIDPN_TARGET_MODE* pMode = NULL;

    status = pModeSetInterface->pfnCreateNewModeInfo(hModeSet, &pMode);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    pMode->VideoSignalInfo.VideoStandard = D3DKMDT_VSS_OTHER;
    pMode->VideoSignalInfo.TotalSize.cx = Width;
    pMode->VideoSignalInfo.TotalSize.cy = Height;
    pMode->VideoSignalInfo.ActiveSize.cx = Width;
    pMode->VideoSignalInfo.ActiveSize.cy = Height;
    pMode->VideoSignalInfo.VSyncFreq.Numerator = RefreshRate;
    pMode->VideoSignalInfo.VSyncFreq.Denominator = 1;
    pMode->VideoSignalInfo.HSyncFreq.Numerator = RefreshRate * Height;
    pMode->VideoSignalInfo.HSyncFreq.Denominator = 1;
    pMode->VideoSignalInfo.PixelRate = (ULONG64)Width * Height * RefreshRate;
    pMode->VideoSignalInfo.ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
    pMode->Preference = D3DKMDT_MP_PREFERRED;

    status = pModeSetInterface->pfnAddMode(hModeSet, pMode);
    if (!NT_SUCCESS(status)) {
        pModeSetInterface->pfnReleaseModeInfo(hModeSet, pMode);
    }

    return status;
}

NTSTATUS
PvgpuIsSupportedVidPn(
    _In_ PVOID MiniportDeviceContext,
    _Inout_ DXGKARG_ISSUPPORTEDVIDPN* IsSupportedVidPn
)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);

    /*
     * We support any VidPn that connects our single source to our single target.
     * For simplicity, we accept all proposed VidPns.
     */
    IsSupportedVidPn->IsVidPnSupported = TRUE;

    KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_TRACE_LEVEL,
        "PVGPU: IsSupportedVidPn - returning TRUE\n"));

    return STATUS_SUCCESS;
}

NTSTATUS
PvgpuRecommendFunctionalVidPn(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_RECOMMENDFUNCTIONALVIDPN* RecommendFunctionalVidPn
)
{
    PPVGPU_DEVICE_CONTEXT context = (PPVGPU_DEVICE_CONTEXT)MiniportDeviceContext;
    NTSTATUS status;
    CONST DXGK_VIDPN_INTERFACE* pVidPnInterface;
    D3DKMDT_HVIDPN hVidPn;
    D3DKMDT_HVIDPNTOPOLOGY hTopology;
    CONST DXGK_VIDPNTOPOLOGY_INTERFACE* pTopologyInterface;
    D3DKMDT_VIDPN_PRESENT_PATH* pPath;

    KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_INFO_LEVEL,
        "PVGPU: RecommendFunctionalVidPn\n"));

    hVidPn = RecommendFunctionalVidPn->hRecommendedFunctionalVidPn;

    /* Get VidPn interface */
    status = context->DxgkInterface.DxgkCbQueryVidPnInterface(
        hVidPn,
        DXGK_VIDPN_INTERFACE_VERSION_V1,
        &pVidPnInterface);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Get topology */
    status = pVidPnInterface->pfnGetTopology(hVidPn, &hTopology, &pTopologyInterface);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Create a present path from source 0 to target 0 */
    status = pTopologyInterface->pfnCreateNewPathInfo(hTopology, &pPath);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    pPath->VidPnSourceId = 0;
    pPath->VidPnTargetId = 0;
    pPath->ImportanceOrdinal = D3DKMDT_VPPI_PRIMARY;
    pPath->ContentTransformation.Scaling = D3DKMDT_VPPS_IDENTITY;
    pPath->ContentTransformation.ScalingSupport.Identity = TRUE;
    pPath->ContentTransformation.Rotation = D3DKMDT_VPPR_IDENTITY;
    pPath->ContentTransformation.RotationSupport.Identity = TRUE;
    pPath->VisibleFromActiveTLOffset.cx = 0;
    pPath->VisibleFromActiveTLOffset.cy = 0;
    pPath->VisibleFromActiveBROffset.cx = 0;
    pPath->VisibleFromActiveBROffset.cy = 0;
    pPath->VidPnTargetColorBasis = D3DKMDT_CB_SRGB;
    pPath->VidPnTargetColorCoeffDynamicRanges.FirstChannel = 8;
    pPath->VidPnTargetColorCoeffDynamicRanges.SecondChannel = 8;
    pPath->VidPnTargetColorCoeffDynamicRanges.ThirdChannel = 8;
    pPath->VidPnTargetColorCoeffDynamicRanges.FourthChannel = 8;
    pPath->Content = D3DKMDT_VPPC_GRAPHICS;
    pPath->CopyProtection.CopyProtectionType = D3DKMDT_VPPMT_NOPROTECTION;
    pPath->GammaRamp.Type = D3DDDI_GAMMARAMP_DEFAULT;

    status = pTopologyInterface->pfnAddPath(hTopology, pPath);
    if (!NT_SUCCESS(status)) {
        pTopologyInterface->pfnReleasePathInfo(hTopology, pPath);
        return status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
PvgpuEnumVidPnCofuncModality(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_ENUMVIDPNCOFUNCMODALITY* EnumCofuncModality
)
{
    PPVGPU_DEVICE_CONTEXT context = (PPVGPU_DEVICE_CONTEXT)MiniportDeviceContext;
    NTSTATUS status;
    CONST DXGK_VIDPN_INTERFACE* pVidPnInterface;
    D3DKMDT_HVIDPN hVidPn;
    D3DKMDT_HVIDPNTOPOLOGY hTopology;
    CONST DXGK_VIDPNTOPOLOGY_INTERFACE* pTopologyInterface;
    D3DKMDT_HVIDPNSOURCEMODESET hSourceModeSet;
    CONST DXGK_VIDPNSOURCEMODESET_INTERFACE* pSourceModeSetInterface;
    D3DKMDT_HVIDPNTARGETMODESET hTargetModeSet;
    CONST DXGK_VIDPNTARGETMODESET_INTERFACE* pTargetModeSetInterface;
    ULONG i;

    KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_INFO_LEVEL,
        "PVGPU: EnumVidPnCofuncModality\n"));

    hVidPn = EnumCofuncModality->hConstrainingVidPn;

    /* Get VidPn interface */
    status = context->DxgkInterface.DxgkCbQueryVidPnInterface(
        hVidPn,
        DXGK_VIDPN_INTERFACE_VERSION_V1,
        &pVidPnInterface);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Get topology */
    status = pVidPnInterface->pfnGetTopology(hVidPn, &hTopology, &pTopologyInterface);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /*
     * Add source modes if the source mode set is not pinned
     */
    status = pVidPnInterface->pfnAcquireSourceModeSet(
        hVidPn,
        0,  /* Source ID */
        &hSourceModeSet,
        &pSourceModeSetInterface);

    if (NT_SUCCESS(status)) {
        CONST D3DKMDT_VIDPN_SOURCE_MODE* pPinnedMode;

        status = pSourceModeSetInterface->pfnAcquirePinnedModeInfo(
            hSourceModeSet,
            &pPinnedMode);

        if (status == STATUS_GRAPHICS_MODE_NOT_PINNED || pPinnedMode == NULL) {
            /* No pinned mode - add all supported modes */
            for (i = 0; i < PVGPU_NUM_DISPLAY_MODES; i++) {
                PvgpuAddSourceMode(
                    hSourceModeSet,
                    pSourceModeSetInterface,
                    g_DisplayModes[i].Width,
                    g_DisplayModes[i].Height);
            }
        } else {
            pSourceModeSetInterface->pfnReleaseModeInfo(hSourceModeSet, pPinnedMode);
        }

        pVidPnInterface->pfnAssignSourceModeSet(hVidPn, 0, hSourceModeSet);
    }

    /*
     * Add target modes if the target mode set is not pinned
     */
    status = pVidPnInterface->pfnAcquireTargetModeSet(
        hVidPn,
        0,  /* Target ID */
        &hTargetModeSet,
        &pTargetModeSetInterface);

    if (NT_SUCCESS(status)) {
        CONST D3DKMDT_VIDPN_TARGET_MODE* pPinnedMode;

        status = pTargetModeSetInterface->pfnAcquirePinnedModeInfo(
            hTargetModeSet,
            &pPinnedMode);

        if (status == STATUS_GRAPHICS_MODE_NOT_PINNED || pPinnedMode == NULL) {
            /* No pinned mode - add all supported modes */
            for (i = 0; i < PVGPU_NUM_DISPLAY_MODES; i++) {
                PvgpuAddTargetMode(
                    hTargetModeSet,
                    pTargetModeSetInterface,
                    g_DisplayModes[i].Width,
                    g_DisplayModes[i].Height,
                    g_DisplayModes[i].RefreshRate);
            }
        } else {
            pTargetModeSetInterface->pfnReleaseModeInfo(hTargetModeSet, pPinnedMode);
        }

        pVidPnInterface->pfnAssignTargetModeSet(hVidPn, 0, hTargetModeSet);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
PvgpuSetVidPnSourceAddress(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_SETVIDPNSOURCEADDRESS* SetVidPnSourceAddress
)
{
    PPVGPU_DEVICE_CONTEXT context = (PPVGPU_DEVICE_CONTEXT)MiniportDeviceContext;

    KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_TRACE_LEVEL,
        "PVGPU: SetVidPnSourceAddress: source=%u, segment=%u, offset=0x%llX\n",
        SetVidPnSourceAddress->VidPnSourceId,
        SetVidPnSourceAddress->PrimarySegment,
        SetVidPnSourceAddress->PrimaryAddress.QuadPart));

    UNREFERENCED_PARAMETER(context);

    return STATUS_SUCCESS;
}

NTSTATUS
PvgpuSetVidPnSourceVisibility(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_SETVIDPNSOURCEVISIBILITY* SetVidPnSourceVisibility
)
{
    KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_TRACE_LEVEL,
        "PVGPU: SetVidPnSourceVisibility: source=%u, visible=%d\n",
        SetVidPnSourceVisibility->VidPnSourceId,
        SetVidPnSourceVisibility->Visible));

    UNREFERENCED_PARAMETER(MiniportDeviceContext);

    return STATUS_SUCCESS;
}

NTSTATUS
PvgpuCommitVidPn(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_COMMITVIDPN* CommitVidPn
)
{
    PPVGPU_DEVICE_CONTEXT context = (PPVGPU_DEVICE_CONTEXT)MiniportDeviceContext;
    NTSTATUS status;
    CONST DXGK_VIDPN_INTERFACE* pVidPnInterface;
    D3DKMDT_HVIDPNSOURCEMODESET hSourceModeSet;
    CONST DXGK_VIDPNSOURCEMODESET_INTERFACE* pSourceModeSetInterface;
    D3DKMDT_HVIDPNTARGETMODESET hTargetModeSet;
    CONST DXGK_VIDPNTARGETMODESET_INTERFACE* pTargetModeSetInterface;
    CONST D3DKMDT_VIDPN_SOURCE_MODE* pSourceMode;
    CONST D3DKMDT_VIDPN_TARGET_MODE* pTargetMode;

    KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_INFO_LEVEL,
        "PVGPU: CommitVidPn\n"));

    /* Get VidPn interface */
    status = context->DxgkInterface.DxgkCbQueryVidPnInterface(
        CommitVidPn->hFunctionalVidPn,
        DXGK_VIDPN_INTERFACE_VERSION_V1,
        &pVidPnInterface);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Get the pinned source mode */
    status = pVidPnInterface->pfnAcquireSourceModeSet(
        CommitVidPn->hFunctionalVidPn,
        0,
        &hSourceModeSet,
        &pSourceModeSetInterface);

    if (NT_SUCCESS(status)) {
        status = pSourceModeSetInterface->pfnAcquirePinnedModeInfo(
            hSourceModeSet,
            &pSourceMode);

        if (NT_SUCCESS(status) && pSourceMode != NULL) {
            context->DisplayWidth = pSourceMode->Format.Graphics.PrimSurfSize.cx;
            context->DisplayHeight = pSourceMode->Format.Graphics.PrimSurfSize.cy;
            pSourceModeSetInterface->pfnReleaseModeInfo(hSourceModeSet, pSourceMode);

            KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_INFO_LEVEL,
                "PVGPU: CommitVidPn: new resolution %ux%u\n",
                context->DisplayWidth, context->DisplayHeight));
        }

        pVidPnInterface->pfnReleaseSourceModeSet(CommitVidPn->hFunctionalVidPn, hSourceModeSet);
    }

    /* Get the pinned target mode for refresh rate */
    status = pVidPnInterface->pfnAcquireTargetModeSet(
        CommitVidPn->hFunctionalVidPn,
        0,
        &hTargetModeSet,
        &pTargetModeSetInterface);

    if (NT_SUCCESS(status)) {
        status = pTargetModeSetInterface->pfnAcquirePinnedModeInfo(
            hTargetModeSet,
            &pTargetMode);

        if (NT_SUCCESS(status) && pTargetMode != NULL) {
            context->DisplayRefresh = pTargetMode->VideoSignalInfo.VSyncFreq.Numerator /
                                      pTargetMode->VideoSignalInfo.VSyncFreq.Denominator;
            pTargetModeSetInterface->pfnReleaseModeInfo(hTargetModeSet, pTargetMode);

            KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_INFO_LEVEL,
                "PVGPU: CommitVidPn: refresh rate %u Hz\n",
                context->DisplayRefresh));
        }

        pVidPnInterface->pfnReleaseTargetModeSet(CommitVidPn->hFunctionalVidPn, hTargetModeSet);
    }

    /* Update the control region with new display settings */
    if (context->ControlRegion) {
        context->ControlRegion->display_width = context->DisplayWidth;
        context->ControlRegion->display_height = context->DisplayHeight;
        context->ControlRegion->display_refresh = context->DisplayRefresh;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
PvgpuUpdateActiveVidPnPresentPath(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH* UpdateActiveVidPnPresentPath
)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(UpdateActiveVidPnPresentPath);

    KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_TRACE_LEVEL,
        "PVGPU: UpdateActiveVidPnPresentPath\n"));

    return STATUS_SUCCESS;
}

NTSTATUS
PvgpuRecommendMonitorModes(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGKARG_RECOMMENDMONITORMODES* RecommendMonitorModes
)
{
    NTSTATUS status;
    D3DKMDT_HMONITORSOURCEMODESET hModeSet;
    CONST DXGK_MONITORSOURCEMODESET_INTERFACE* pModeSetInterface;
    D3DKMDT_MONITOR_SOURCE_MODE* pMode;
    ULONG i;

    UNREFERENCED_PARAMETER(MiniportDeviceContext);

    KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_INFO_LEVEL,
        "PVGPU: RecommendMonitorModes\n"));

    hModeSet = RecommendMonitorModes->hMonitorSourceModeSet;
    pModeSetInterface = RecommendMonitorModes->pMonitorSourceModeSetInterface;

    /* Add all supported display modes */
    for (i = 0; i < PVGPU_NUM_DISPLAY_MODES; i++) {
        status = pModeSetInterface->pfnCreateNewModeInfo(hModeSet, &pMode);
        if (!NT_SUCCESS(status)) {
            continue;
        }

        pMode->VideoSignalInfo.VideoStandard = D3DKMDT_VSS_OTHER;
        pMode->VideoSignalInfo.TotalSize.cx = g_DisplayModes[i].Width;
        pMode->VideoSignalInfo.TotalSize.cy = g_DisplayModes[i].Height;
        pMode->VideoSignalInfo.ActiveSize.cx = g_DisplayModes[i].Width;
        pMode->VideoSignalInfo.ActiveSize.cy = g_DisplayModes[i].Height;
        pMode->VideoSignalInfo.VSyncFreq.Numerator = g_DisplayModes[i].RefreshRate;
        pMode->VideoSignalInfo.VSyncFreq.Denominator = 1;
        pMode->VideoSignalInfo.HSyncFreq.Numerator = g_DisplayModes[i].RefreshRate * g_DisplayModes[i].Height;
        pMode->VideoSignalInfo.HSyncFreq.Denominator = 1;
        pMode->VideoSignalInfo.PixelRate = (ULONG64)g_DisplayModes[i].Width * 
                                           g_DisplayModes[i].Height * 
                                           g_DisplayModes[i].RefreshRate;
        pMode->VideoSignalInfo.ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
        pMode->ColorBasis = D3DKMDT_CB_SRGB;
        pMode->ColorCoeffDynamicRanges.FirstChannel = 8;
        pMode->ColorCoeffDynamicRanges.SecondChannel = 8;
        pMode->ColorCoeffDynamicRanges.ThirdChannel = 8;
        pMode->ColorCoeffDynamicRanges.FourthChannel = 8;
        pMode->Origin = D3DKMDT_MCO_DRIVER;
        pMode->Preference = (i == 2) ? D3DKMDT_MP_PREFERRED : D3DKMDT_MP_NOTPREFERRED;  /* 1080p60 preferred */

        status = pModeSetInterface->pfnAddMode(hModeSet, pMode);
        if (!NT_SUCCESS(status)) {
            pModeSetInterface->pfnReleaseModeInfo(hModeSet, pMode);
        }
    }

    return STATUS_SUCCESS;
}
