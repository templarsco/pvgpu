/*
 * PVGPU User-Mode Driver (UMD) - Implementation
 *
 * D3D11 User-Mode Display Driver that translates D3D11 API calls into
 * pvgpu commands submitted to the kernel-mode driver.
 *
 * Copyright (c) SANSI-GROUP. All rights reserved.
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d10umddi.h>
#include <d3d11_1.h>

#include "pvgpu_umd.h"

/* ============================================================================
 * Debug Helpers
 * ============================================================================ */

#ifdef _DEBUG
#define PVGPU_TRACE(fmt, ...) OutputDebugStringA("[PVGPU-UMD] " fmt "\n")
#else
#define PVGPU_TRACE(fmt, ...)
#endif

/* ============================================================================
 * DLL Entry Point
 * ============================================================================ */

BOOL WINAPI DllMain(
    HINSTANCE hinstDLL,
    DWORD fdwReason,
    LPVOID lpvReserved)
{
    UNREFERENCED_PARAMETER(hinstDLL);
    UNREFERENCED_PARAMETER(lpvReserved);

    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        PVGPU_TRACE("DLL_PROCESS_ATTACH");
        DisableThreadLibraryCalls(hinstDLL);
        break;

    case DLL_PROCESS_DETACH:
        PVGPU_TRACE("DLL_PROCESS_DETACH");
        break;
    }

    return TRUE;
}

/* ============================================================================
 * OpenAdapter - Main DDI Entry Point
 * ============================================================================ */

/*
 * OpenAdapter10_2 - Called by D3D runtime to initialize the adapter
 *
 * This is the main entry point exported by the UMD DLL. The runtime calls
 * this to get function pointers for adapter-level operations.
 */
HRESULT APIENTRY OpenAdapter10_2(
    _Inout_ D3D10DDIARG_OPENADAPTER* pOpenData)
{
    PVGPU_UMD_ADAPTER* pAdapter;
    
    PVGPU_TRACE("OpenAdapter10_2 called");
    
    if (pOpenData == NULL)
    {
        return E_INVALIDARG;
    }
    
    /* Validate interface version */
    if (pOpenData->Interface < D3D10_1_DDI_INTERFACE_VERSION)
    {
        PVGPU_TRACE("Unsupported interface version: 0x%x", pOpenData->Interface);
        return E_NOINTERFACE;
    }
    
    /* Allocate adapter structure */
    pAdapter = (PVGPU_UMD_ADAPTER*)HeapAlloc(
        GetProcessHeap(),
        HEAP_ZERO_MEMORY,
        sizeof(PVGPU_UMD_ADAPTER));
    
    if (pAdapter == NULL)
    {
        return E_OUTOFMEMORY;
    }
    
    /* Store runtime handles */
    pAdapter->hRTAdapter = pOpenData->hRTAdapter;
    pAdapter->pAdapterCallbacks = pOpenData->pAdapterCallbacks;
    
    /* Initialize adapter capabilities */
    pAdapter->MaxTextureWidth = 16384;
    pAdapter->MaxTextureHeight = 16384;
    pAdapter->MaxTexture3DDepth = 2048;
    pAdapter->MaxTextureCubeSize = 16384;
    pAdapter->MaxPrimitiveCount = 0xFFFFFFFF;
    pAdapter->SupportsCompute = FALSE;      /* TODO: Enable when implemented */
    pAdapter->SupportsTessellation = FALSE; /* TODO: Enable when implemented */
    pAdapter->SupportsStreamOutput = FALSE; /* TODO: Enable when implemented */
    
    /* Fill in adapter function table */
    pOpenData->pAdapterFuncs->pfnCalcPrivateDeviceSize = PvgpuCalcPrivateDeviceSize;
    pOpenData->pAdapterFuncs->pfnCreateDevice = PvgpuCreateDevice;
    pOpenData->pAdapterFuncs->pfnCloseAdapter = PvgpuCloseAdapter;
    
    /* Fill in adapter callbacks for D3D10.2+ */
    pOpenData->pAdapterFuncs_2->pfnGetCaps = PvgpuGetCaps;
    pOpenData->pAdapterFuncs_2->pfnGetSupportedVersions = PvgpuGetSupportedVersions;
    
    /* Return our adapter handle */
    pOpenData->hAdapter.pDrvPrivate = pAdapter;
    
    PVGPU_TRACE("OpenAdapter10_2 succeeded");
    return S_OK;
}

/* ============================================================================
 * Adapter Functions
 * ============================================================================ */

SIZE_T APIENTRY PvgpuCalcPrivateDeviceSize(
    _In_ D3D10DDI_HADAPTER hAdapter,
    _In_ CONST D3D10DDIARG_CALCPRIVATEDEVICESIZE* pCalcPrivateDeviceSize)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pCalcPrivateDeviceSize);
    
    return sizeof(PVGPU_UMD_DEVICE);
}

HRESULT APIENTRY PvgpuCreateDevice(
    _In_ D3D10DDI_HADAPTER hAdapter,
    _Inout_ D3D10DDIARG_CREATEDEVICE* pCreateData)
{
    PVGPU_UMD_ADAPTER* pAdapter;
    PVGPU_UMD_DEVICE* pDevice;
    D3D10DDI_DEVICEFUNCS* pDeviceFuncs;
    
    PVGPU_TRACE("PvgpuCreateDevice called");
    
    pAdapter = (PVGPU_UMD_ADAPTER*)hAdapter.pDrvPrivate;
    pDevice = (PVGPU_UMD_DEVICE*)pCreateData->hDrvDevice.pDrvPrivate;
    
    if (pDevice == NULL)
    {
        return E_INVALIDARG;
    }
    
    /* Initialize device structure */
    ZeroMemory(pDevice, sizeof(PVGPU_UMD_DEVICE));
    pDevice->hRTDevice = pCreateData->hRTDevice;
    pDevice->hRTCoreLayer = pCreateData->hRTCoreLayer;
    pDevice->pRTCallbacks = pCreateData->pKTCallbacks ? NULL : pCreateData->p10_1DeviceCallbacks;
    pDevice->pKTCallbacks = pCreateData->pKTCallbacks;
    pDevice->pAdapter = pAdapter;
    
    /* Initialize resource tracking */
    pDevice->pResources = (PVGPU_UMD_RESOURCE*)HeapAlloc(
        GetProcessHeap(),
        HEAP_ZERO_MEMORY,
        sizeof(PVGPU_UMD_RESOURCE) * PVGPU_UMD_MAX_RESOURCES);
    
    if (pDevice->pResources == NULL)
    {
        return E_OUTOFMEMORY;
    }
    
    pDevice->ResourceCount = 0;
    pDevice->NextResourceHandle = 1; /* 0 is reserved for NULL */
    InitializeCriticalSection(&pDevice->ResourceLock);
    
    /* Allocate command buffer */
    pDevice->pCommandBuffer = (UINT8*)HeapAlloc(
        GetProcessHeap(),
        HEAP_ZERO_MEMORY,
        PVGPU_UMD_COMMAND_BUFFER_SIZE);
    
    if (pDevice->pCommandBuffer == NULL)
    {
        HeapFree(GetProcessHeap(), 0, pDevice->pResources);
        return E_OUTOFMEMORY;
    }
    
    pDevice->CommandBufferSize = PVGPU_UMD_COMMAND_BUFFER_SIZE;
    pDevice->CommandBufferOffset = 0;
    
    /* Fill in device function table */
    pDeviceFuncs = pCreateData->pDeviceFuncs;
    
    /* Device management */
    pDeviceFuncs->pfnDestroyDevice = PvgpuDestroyDevice;
    pDeviceFuncs->pfnFlush = PvgpuFlush;
    
    /* Resource creation/destruction */
    pDeviceFuncs->pfnCalcPrivateResourceSize = PvgpuCalcPrivateResourceSize;
    pDeviceFuncs->pfnCreateResource = PvgpuCreateResource;
    pDeviceFuncs->pfnDestroyResource = PvgpuDestroyResource;
    pDeviceFuncs->pfnOpenResource = PvgpuOpenResource;
    
    /* Shader creation */
    pDeviceFuncs->pfnCalcPrivateShaderSize = PvgpuCalcPrivateShaderSize;
    pDeviceFuncs->pfnCreateVertexShader = PvgpuCreateVertexShader;
    pDeviceFuncs->pfnCreatePixelShader = PvgpuCreatePixelShader;
    pDeviceFuncs->pfnCreateGeometryShader = PvgpuCreateGeometryShader;
    pDeviceFuncs->pfnDestroyShader = PvgpuDestroyShader;
    
    /* Input assembler */
    pDeviceFuncs->pfnIaSetInputLayout = PvgpuIaSetInputLayout;
    pDeviceFuncs->pfnIaSetVertexBuffers = PvgpuIaSetVertexBuffers;
    pDeviceFuncs->pfnIaSetIndexBuffer = PvgpuIaSetIndexBuffer;
    pDeviceFuncs->pfnIaSetTopology = PvgpuIaSetTopology;
    
    /* Shader stages */
    pDeviceFuncs->pfnVsSetShader = PvgpuVsSetShader;
    pDeviceFuncs->pfnPsSetShader = PvgpuPsSetShader;
    pDeviceFuncs->pfnGsSetShader = PvgpuGsSetShader;
    
    /* Output merger */
    pDeviceFuncs->pfnSetRenderTargets = PvgpuSetRenderTargets;
    pDeviceFuncs->pfnSetBlendState = PvgpuSetBlendState;
    pDeviceFuncs->pfnSetDepthStencilState = PvgpuSetDepthStencilState;
    
    /* Rasterizer */
    pDeviceFuncs->pfnSetViewports = PvgpuSetViewports;
    pDeviceFuncs->pfnSetScissorRects = PvgpuSetScissorRects;
    pDeviceFuncs->pfnSetRasterizerState = PvgpuSetRasterizerState;
    
    /* Draw commands */
    pDeviceFuncs->pfnDraw = PvgpuDraw;
    pDeviceFuncs->pfnDrawIndexed = PvgpuDrawIndexed;
    pDeviceFuncs->pfnDrawInstanced = PvgpuDrawInstanced;
    pDeviceFuncs->pfnDrawIndexedInstanced = PvgpuDrawIndexedInstanced;
    pDeviceFuncs->pfnDrawAuto = PvgpuDrawAuto;
    
    /* Clear commands */
    pDeviceFuncs->pfnClearRenderTargetView = PvgpuClearRenderTargetView;
    pDeviceFuncs->pfnClearDepthStencilView = PvgpuClearDepthStencilView;
    
    /* Resource operations */
    pDeviceFuncs->pfnResourceCopy = PvgpuResourceCopy;
    pDeviceFuncs->pfnResourceCopyRegion = PvgpuResourceCopyRegion;
    pDeviceFuncs->pfnResourceUpdateSubresourceUP = PvgpuResourceUpdateSubresourceUP;
    pDeviceFuncs->pfnResourceMap = PvgpuResourceMap;
    pDeviceFuncs->pfnResourceUnmap = PvgpuResourceUnmap;
    
    PVGPU_TRACE("PvgpuCreateDevice succeeded");
    return S_OK;
}

HRESULT APIENTRY PvgpuCloseAdapter(
    _In_ D3D10DDI_HADAPTER hAdapter)
{
    PVGPU_UMD_ADAPTER* pAdapter;
    
    PVGPU_TRACE("PvgpuCloseAdapter called");
    
    pAdapter = (PVGPU_UMD_ADAPTER*)hAdapter.pDrvPrivate;
    
    if (pAdapter != NULL)
    {
        HeapFree(GetProcessHeap(), 0, pAdapter);
    }
    
    return S_OK;
}

HRESULT APIENTRY PvgpuGetCaps(
    _In_ D3D10DDI_HADAPTER hAdapter,
    _In_ CONST D3D10_2DDIARG_GETCAPS* pData)
{
    PVGPU_UMD_ADAPTER* pAdapter;
    
    pAdapter = (PVGPU_UMD_ADAPTER*)hAdapter.pDrvPrivate;
    
    switch (pData->Type)
    {
    case D3D10_2DDICAPS_TYPE_GETFORMATCOUNT:
        /* Return number of formats we support */
        if (pData->pData && pData->DataSize >= sizeof(UINT))
        {
            *(UINT*)pData->pData = 100; /* Approximate format count */
        }
        break;
        
    case D3D10_2DDICAPS_TYPE_GETFORMATDATA:
        /* TODO: Fill in format support data */
        break;
        
    case D3D10_2DDICAPS_TYPE_GETMULTISAMPLEQUALITYLEVELS:
        /* We support 1x MSAA only for now */
        if (pData->pData && pData->DataSize >= sizeof(UINT))
        {
            *(UINT*)pData->pData = 1;
        }
        break;
        
    case D3D11DDICAPS_THREADING:
        /* No driver-level threading support */
        if (pData->pData && pData->DataSize >= sizeof(D3D11DDI_THREADING_CAPS))
        {
            D3D11DDI_THREADING_CAPS* pCaps = (D3D11DDI_THREADING_CAPS*)pData->pData;
            pCaps->Caps = 0;
        }
        break;
        
    case D3D11DDICAPS_3DPIPELINESUPPORT:
        /* Report D3D11 pipeline support */
        if (pData->pData && pData->DataSize >= sizeof(D3D11DDI_3DPIPELINESUPPORT_CAPS))
        {
            D3D11DDI_3DPIPELINESUPPORT_CAPS* pCaps = (D3D11DDI_3DPIPELINESUPPORT_CAPS*)pData->pData;
            pCaps->Caps = D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_11_0);
        }
        break;
        
    default:
        PVGPU_TRACE("GetCaps: Unhandled type %d", pData->Type);
        break;
    }
    
    return S_OK;
}

HRESULT APIENTRY PvgpuGetSupportedVersions(
    _In_ D3D10DDI_HADAPTER hAdapter,
    _Inout_ UINT32* puEntries,
    _Out_writes_opt_(*puEntries) UINT64* pSupportedDDIInterfaceVersions)
{
    static const UINT64 SupportedVersions[] = {
        D3D11_1_DDI_INTERFACE_VERSION,
        D3D11_0_DDI_INTERFACE_VERSION,
        D3D10_1_DDI_INTERFACE_VERSION,
    };
    
    UNREFERENCED_PARAMETER(hAdapter);
    
    if (pSupportedDDIInterfaceVersions == NULL)
    {
        /* Return count only */
        *puEntries = ARRAYSIZE(SupportedVersions);
        return S_OK;
    }
    
    /* Copy versions */
    for (UINT32 i = 0; i < *puEntries && i < ARRAYSIZE(SupportedVersions); i++)
    {
        pSupportedDDIInterfaceVersions[i] = SupportedVersions[i];
    }
    
    *puEntries = min(*puEntries, ARRAYSIZE(SupportedVersions));
    return S_OK;
}

/* ============================================================================
 * Device Functions
 * ============================================================================ */

void APIENTRY PvgpuDestroyDevice(
    _In_ D3D10DDI_HDEVICE hDevice)
{
    PVGPU_UMD_DEVICE* pDevice;
    
    PVGPU_TRACE("PvgpuDestroyDevice called");
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    
    if (pDevice != NULL)
    {
        /* Flush any pending commands */
        PvgpuFlushCommandBuffer(pDevice);
        
        /* Free command buffer */
        if (pDevice->pCommandBuffer != NULL)
        {
            HeapFree(GetProcessHeap(), 0, pDevice->pCommandBuffer);
        }
        
        /* Free resource tracking */
        if (pDevice->pResources != NULL)
        {
            HeapFree(GetProcessHeap(), 0, pDevice->pResources);
        }
        
        DeleteCriticalSection(&pDevice->ResourceLock);
        
        PVGPU_TRACE("Device destroyed: %llu draw calls, %llu commands",
            pDevice->DrawCallCount, pDevice->CommandsSubmitted);
    }
}

void APIENTRY PvgpuFlush(
    _In_ D3D10DDI_HDEVICE hDevice)
{
    PVGPU_UMD_DEVICE* pDevice;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    
    if (pDevice != NULL)
    {
        PvgpuFlushCommandBuffer(pDevice);
    }
}

/* ============================================================================
 * Resource Creation/Destruction
 * ============================================================================ */

SIZE_T APIENTRY PvgpuCalcPrivateResourceSize(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10DDIARG_CREATERESOURCE* pCreateResource)
{
    UNREFERENCED_PARAMETER(hDevice);
    UNREFERENCED_PARAMETER(pCreateResource);
    
    return sizeof(PVGPU_UMD_RESOURCE);
}

void APIENTRY PvgpuCreateResource(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10DDIARG_CREATERESOURCE* pCreateResource,
    _In_ D3D10DDI_HRESOURCE hResource,
    _In_ D3D10DDI_HRTRESOURCE hRTResource)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_RESOURCE* pResource;
    PvgpuCmdCreateResource cmd;
    
    UNREFERENCED_PARAMETER(hRTResource);
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pResource = (PVGPU_UMD_RESOURCE*)hResource.pDrvPrivate;
    
    if (pResource == NULL)
    {
        return;
    }
    
    /* Initialize resource tracking structure */
    ZeroMemory(pResource, sizeof(PVGPU_UMD_RESOURCE));
    
    /* Determine resource type */
    switch (pCreateResource->ResourceDimension)
    {
    case D3D10DDIRESOURCE_BUFFER:
        pResource->Type = PVGPU_RESOURCE_TYPE_BUFFER;
        pResource->ByteWidth = pCreateResource->pMipInfoList[0].TexelWidth;
        break;
        
    case D3D10DDIRESOURCE_TEXTURE1D:
        pResource->Type = PVGPU_RESOURCE_TYPE_TEXTURE1D;
        break;
        
    case D3D10DDIRESOURCE_TEXTURE2D:
        pResource->Type = PVGPU_RESOURCE_TYPE_TEXTURE2D;
        break;
        
    case D3D10DDIRESOURCE_TEXTURE3D:
        pResource->Type = PVGPU_RESOURCE_TYPE_TEXTURE3D;
        break;
        
    default:
        pResource->Type = PVGPU_RESOURCE_TYPE_UNKNOWN;
        break;
    }
    
    /* Store resource properties */
    if (pCreateResource->pMipInfoList != NULL)
    {
        pResource->Width = pCreateResource->pMipInfoList[0].TexelWidth;
        pResource->Height = pCreateResource->pMipInfoList[0].TexelHeight;
        pResource->Depth = pCreateResource->pMipInfoList[0].TexelDepth;
    }
    pResource->MipLevels = pCreateResource->MipLevels;
    pResource->ArraySize = pCreateResource->ArraySize;
    pResource->Format = pCreateResource->Format;
    pResource->BindFlags = pCreateResource->BindFlags;
    pResource->MiscFlags = pCreateResource->MiscFlags;
    
    /* Allocate a host handle */
    pResource->HostHandle = PvgpuAllocateResourceHandle(pDevice);
    
    /* Build create resource command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.resource_id = pResource->HostHandle;
    cmd.width = pResource->Width;
    cmd.height = pResource->Height;
    cmd.depth = pResource->Depth;
    cmd.mip_levels = pResource->MipLevels;
    cmd.array_size = pResource->ArraySize;
    cmd.format = pResource->Format;
    cmd.bind_flags = pResource->BindFlags;
    
    /* Determine resource type for protocol */
    switch (pResource->Type)
    {
    case PVGPU_RESOURCE_TYPE_BUFFER:
        cmd.resource_type = PVGPU_RESOURCE_BUFFER;
        break;
    case PVGPU_RESOURCE_TYPE_TEXTURE1D:
        cmd.resource_type = PVGPU_RESOURCE_TEXTURE_1D;
        break;
    case PVGPU_RESOURCE_TYPE_TEXTURE2D:
        cmd.resource_type = PVGPU_RESOURCE_TEXTURE_2D;
        break;
    case PVGPU_RESOURCE_TYPE_TEXTURE3D:
        cmd.resource_type = PVGPU_RESOURCE_TEXTURE_3D;
        break;
    default:
        cmd.resource_type = PVGPU_RESOURCE_BUFFER;
        break;
    }
    
    /* Submit command to host */
    PvgpuWriteCommand(pDevice, PVGPU_CMD_CREATE_RESOURCE, &cmd, sizeof(cmd));
    
    PVGPU_TRACE("Created resource %u: %ux%u format=%u",
        pResource->HostHandle, pResource->Width, pResource->Height, pResource->Format);
}

void APIENTRY PvgpuDestroyResource(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HRESOURCE hResource)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_RESOURCE* pResource;
    PvgpuCmdDestroyResource cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pResource = (PVGPU_UMD_RESOURCE*)hResource.pDrvPrivate;
    
    if (pResource == NULL || pResource->HostHandle == 0)
    {
        return;
    }
    
    /* Build destroy command */
    cmd.resource_id = pResource->HostHandle;
    
    /* Submit command */
    PvgpuWriteCommand(pDevice, PVGPU_CMD_DESTROY_RESOURCE, &cmd, sizeof(cmd));
    
    PVGPU_TRACE("Destroyed resource %u", pResource->HostHandle);
}

void APIENTRY PvgpuOpenResource(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10DDIARG_OPENRESOURCE* pOpenResource,
    _In_ D3D10DDI_HRESOURCE hResource,
    _In_ D3D10DDI_HRTRESOURCE hRTResource)
{
    /* TODO: Implement shared resource opening */
    UNREFERENCED_PARAMETER(hDevice);
    UNREFERENCED_PARAMETER(pOpenResource);
    UNREFERENCED_PARAMETER(hResource);
    UNREFERENCED_PARAMETER(hRTResource);
    
    PVGPU_TRACE("OpenResource: Not implemented");
}

/* ============================================================================
 * Shader Creation
 * ============================================================================ */

SIZE_T APIENTRY PvgpuCalcPrivateShaderSize(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST UINT* pCode,
    _In_ CONST D3D10DDIARG_STAGE_IO_SIGNATURES* pSignatures)
{
    UNREFERENCED_PARAMETER(hDevice);
    UNREFERENCED_PARAMETER(pCode);
    UNREFERENCED_PARAMETER(pSignatures);
    
    return sizeof(PVGPU_UMD_SHADER);
}

static void PvgpuCreateShaderInternal(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST UINT* pCode,
    _In_ D3D10DDI_HSHADER hShader,
    _In_ PVGPU_SHADER_TYPE shaderType)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_SHADER* pShader;
    PvgpuCmdCreateShader cmd;
    SIZE_T bytecodeSize;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pShader = (PVGPU_UMD_SHADER*)hShader.pDrvPrivate;
    
    if (pShader == NULL || pCode == NULL)
    {
        return;
    }
    
    /* Get bytecode size from DXBC header */
    bytecodeSize = pCode[6]; /* DXBC size is at offset 24 (index 6) */
    
    /* Initialize shader structure */
    ZeroMemory(pShader, sizeof(PVGPU_UMD_SHADER));
    pShader->Type = shaderType;
    pShader->HostHandle = PvgpuAllocateResourceHandle(pDevice);
    pShader->BytecodeSize = bytecodeSize;
    
    /* Build create shader command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.shader_id = pShader->HostHandle;
    cmd.shader_type = shaderType;
    cmd.bytecode_size = (UINT32)bytecodeSize;
    cmd.bytecode_offset = 0; /* Will be set when we copy bytecode to shared memory */
    
    /* Submit command */
    PvgpuWriteCommand(pDevice, PVGPU_CMD_CREATE_SHADER, &cmd, sizeof(cmd));
    
    /* TODO: Copy bytecode to shared memory for host to compile */
    
    PVGPU_TRACE("Created shader %u type=%d size=%zu",
        pShader->HostHandle, shaderType, bytecodeSize);
}

void APIENTRY PvgpuCreateVertexShader(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST UINT* pCode,
    _In_ D3D10DDI_HSHADER hShader,
    _In_ D3D10DDI_HRTSHADER hRTShader,
    _In_ CONST D3D10DDIARG_STAGE_IO_SIGNATURES* pSignatures)
{
    UNREFERENCED_PARAMETER(hRTShader);
    UNREFERENCED_PARAMETER(pSignatures);
    
    PvgpuCreateShaderInternal(hDevice, pCode, hShader, PVGPU_SHADER_VERTEX);
}

void APIENTRY PvgpuCreatePixelShader(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST UINT* pCode,
    _In_ D3D10DDI_HSHADER hShader,
    _In_ D3D10DDI_HRTSHADER hRTShader,
    _In_ CONST D3D10DDIARG_STAGE_IO_SIGNATURES* pSignatures)
{
    UNREFERENCED_PARAMETER(hRTShader);
    UNREFERENCED_PARAMETER(pSignatures);
    
    PvgpuCreateShaderInternal(hDevice, pCode, hShader, PVGPU_SHADER_PIXEL);
}

void APIENTRY PvgpuCreateGeometryShader(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST UINT* pCode,
    _In_ D3D10DDI_HSHADER hShader,
    _In_ D3D10DDI_HRTSHADER hRTShader,
    _In_ CONST D3D10DDIARG_STAGE_IO_SIGNATURES* pSignatures)
{
    UNREFERENCED_PARAMETER(hRTShader);
    UNREFERENCED_PARAMETER(pSignatures);
    
    PvgpuCreateShaderInternal(hDevice, pCode, hShader, PVGPU_SHADER_GEOMETRY);
}

void APIENTRY PvgpuDestroyShader(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HSHADER hShader)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_SHADER* pShader;
    PvgpuCmdDestroyShader cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pShader = (PVGPU_UMD_SHADER*)hShader.pDrvPrivate;
    
    if (pShader == NULL || pShader->HostHandle == 0)
    {
        return;
    }
    
    cmd.shader_id = pShader->HostHandle;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_DESTROY_SHADER, &cmd, sizeof(cmd));
    
    PVGPU_TRACE("Destroyed shader %u", pShader->HostHandle);
}

/* ============================================================================
 * Draw Commands
 * ============================================================================ */

void APIENTRY PvgpuDraw(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT VertexCount,
    _In_ UINT StartVertexLocation)
{
    PVGPU_UMD_DEVICE* pDevice;
    PvgpuCmdDraw cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.vertex_count = VertexCount;
    cmd.start_vertex = StartVertexLocation;
    cmd.instance_count = 1;
    cmd.start_instance = 0;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_DRAW, &cmd, sizeof(cmd));
    
    pDevice->DrawCallCount++;
}

void APIENTRY PvgpuDrawIndexed(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT IndexCount,
    _In_ UINT StartIndexLocation,
    _In_ INT BaseVertexLocation)
{
    PVGPU_UMD_DEVICE* pDevice;
    PvgpuCmdDrawIndexed cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.index_count = IndexCount;
    cmd.start_index = StartIndexLocation;
    cmd.base_vertex = BaseVertexLocation;
    cmd.instance_count = 1;
    cmd.start_instance = 0;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_DRAW_INDEXED, &cmd, sizeof(cmd));
    
    pDevice->DrawCallCount++;
}

void APIENTRY PvgpuDrawInstanced(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT VertexCountPerInstance,
    _In_ UINT InstanceCount,
    _In_ UINT StartVertexLocation,
    _In_ UINT StartInstanceLocation)
{
    PVGPU_UMD_DEVICE* pDevice;
    PvgpuCmdDraw cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.vertex_count = VertexCountPerInstance;
    cmd.start_vertex = StartVertexLocation;
    cmd.instance_count = InstanceCount;
    cmd.start_instance = StartInstanceLocation;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_DRAW, &cmd, sizeof(cmd));
    
    pDevice->DrawCallCount++;
}

void APIENTRY PvgpuDrawIndexedInstanced(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT IndexCountPerInstance,
    _In_ UINT InstanceCount,
    _In_ UINT StartIndexLocation,
    _In_ INT BaseVertexLocation,
    _In_ UINT StartInstanceLocation)
{
    PVGPU_UMD_DEVICE* pDevice;
    PvgpuCmdDrawIndexed cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.index_count = IndexCountPerInstance;
    cmd.start_index = StartIndexLocation;
    cmd.base_vertex = BaseVertexLocation;
    cmd.instance_count = InstanceCount;
    cmd.start_instance = StartInstanceLocation;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_DRAW_INDEXED, &cmd, sizeof(cmd));
    
    pDevice->DrawCallCount++;
}

void APIENTRY PvgpuDrawAuto(
    _In_ D3D10DDI_HDEVICE hDevice)
{
    /* DrawAuto uses stream output to determine vertex count */
    /* TODO: Implement when stream output is supported */
    UNREFERENCED_PARAMETER(hDevice);
    
    PVGPU_TRACE("DrawAuto: Not implemented");
}

/* ============================================================================
 * Clear Commands
 * ============================================================================ */

void APIENTRY PvgpuClearRenderTargetView(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HRENDERTARGETVIEW hRenderTargetView,
    _In_reads_(4) CONST FLOAT ColorRGBA[4])
{
    PVGPU_UMD_DEVICE* pDevice;
    PvgpuCmdClearRenderTarget cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    
    /* Get the resource handle from the RTV */
    /* For now, use 0 as a placeholder - need proper view tracking */
    
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.render_target_id = 0; /* TODO: Get from RTV tracking */
    cmd.color[0] = ColorRGBA[0];
    cmd.color[1] = ColorRGBA[1];
    cmd.color[2] = ColorRGBA[2];
    cmd.color[3] = ColorRGBA[3];
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_CLEAR_RENDER_TARGET, &cmd, sizeof(cmd));
}

void APIENTRY PvgpuClearDepthStencilView(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HDEPTHSTENCILVIEW hDepthStencilView,
    _In_ UINT ClearFlags,
    _In_ FLOAT Depth,
    _In_ UINT8 Stencil)
{
    PVGPU_UMD_DEVICE* pDevice;
    PvgpuCmdClearDepthStencil cmd;
    
    UNREFERENCED_PARAMETER(hDepthStencilView);
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.depth_stencil_id = 0; /* TODO: Get from DSV tracking */
    cmd.clear_flags = ClearFlags;
    cmd.depth = Depth;
    cmd.stencil = Stencil;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_CLEAR_DEPTH_STENCIL, &cmd, sizeof(cmd));
}

/* ============================================================================
 * Pipeline State (Stubs - need implementation)
 * ============================================================================ */

void APIENTRY PvgpuIaSetInputLayout(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HELEMENTLAYOUT hInputLayout)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_RESOURCE* pLayout;
    PvgpuCommandHeader cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pLayout = (PVGPU_UMD_RESOURCE*)hInputLayout.pDrvPrivate;
    
    /* Track current input layout */
    pDevice->PipelineState.InputLayout = pLayout ? pLayout->HostHandle : 0;
    
    /* Build and submit command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.command_type = PVGPU_CMD_SET_INPUT_LAYOUT;
    cmd.command_size = sizeof(cmd);
    cmd.resource_id = pDevice->PipelineState.InputLayout;
    cmd.flags = 0;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_SET_INPUT_LAYOUT, &cmd, sizeof(cmd));
}

void APIENTRY PvgpuIaSetVertexBuffers(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT StartBuffer,
    _In_ UINT NumBuffers,
    _In_reads_(NumBuffers) CONST D3D10DDI_HRESOURCE* phBuffers,
    _In_reads_(NumBuffers) CONST UINT* pStrides,
    _In_reads_(NumBuffers) CONST UINT* pOffsets)
{
    PVGPU_UMD_DEVICE* pDevice;
    PvgpuCmdSetVertexBuffer cmd;
    UINT i;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    
    /* Limit to maximum supported */
    if (NumBuffers > 16) NumBuffers = 16;
    
    /* Build command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_SET_VERTEX_BUFFER;
    cmd.header.command_size = sizeof(cmd);
    cmd.start_slot = StartBuffer;
    cmd.num_buffers = NumBuffers;
    
    for (i = 0; i < NumBuffers; i++)
    {
        PVGPU_UMD_RESOURCE* pBuffer = (PVGPU_UMD_RESOURCE*)phBuffers[i].pDrvPrivate;
        cmd.buffers[i].buffer_id = pBuffer ? pBuffer->HostHandle : 0;
        cmd.buffers[i].stride = pStrides[i];
        cmd.buffers[i].offset = pOffsets[i];
        
        /* Track in device state */
        if (StartBuffer + i < PVGPU_UMD_MAX_VERTEX_BUFFERS)
        {
            pDevice->PipelineState.VertexBuffers[StartBuffer + i] = cmd.buffers[i].buffer_id;
            pDevice->PipelineState.VertexBufferStrides[StartBuffer + i] = pStrides[i];
            pDevice->PipelineState.VertexBufferOffsets[StartBuffer + i] = pOffsets[i];
        }
    }
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_SET_VERTEX_BUFFER, &cmd, sizeof(cmd));
}

void APIENTRY PvgpuIaSetIndexBuffer(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HRESOURCE hBuffer,
    _In_ DXGI_FORMAT Format,
    _In_ UINT Offset)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_RESOURCE* pBuffer;
    PvgpuCmdSetIndexBuffer cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pBuffer = (PVGPU_UMD_RESOURCE*)hBuffer.pDrvPrivate;
    
    /* Track in device state */
    pDevice->PipelineState.IndexBuffer = pBuffer ? pBuffer->HostHandle : 0;
    pDevice->PipelineState.IndexBufferFormat = Format;
    pDevice->PipelineState.IndexBufferOffset = Offset;
    
    /* Build command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_SET_INDEX_BUFFER;
    cmd.header.command_size = sizeof(cmd);
    cmd.buffer_id = pDevice->PipelineState.IndexBuffer;
    cmd.format = Format;
    cmd.offset = Offset;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_SET_INDEX_BUFFER, &cmd, sizeof(cmd));
}

void APIENTRY PvgpuIaSetTopology(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10_DDI_PRIMITIVE_TOPOLOGY PrimitiveTopology)
{
    PVGPU_UMD_DEVICE* pDevice;
    PvgpuCmdSetPrimitiveTopology cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    
    /* Track in device state */
    pDevice->PipelineState.PrimitiveTopology = (UINT32)PrimitiveTopology;
    
    /* Build command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_SET_PRIMITIVE_TOPOLOGY;
    cmd.header.command_size = sizeof(cmd);
    cmd.topology = (uint32_t)PrimitiveTopology;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_SET_PRIMITIVE_TOPOLOGY, &cmd, sizeof(cmd));
}

void APIENTRY PvgpuVsSetShader(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HSHADER hShader)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_SHADER* pShader;
    PvgpuCmdSetShader cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pShader = (PVGPU_UMD_SHADER*)hShader.pDrvPrivate;
    
    /* Track in device state */
    pDevice->PipelineState.VertexShader = pShader ? pShader->HostHandle : 0;
    
    /* Build command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_SET_SHADER;
    cmd.header.command_size = sizeof(cmd);
    cmd.stage = PVGPU_STAGE_VERTEX;
    cmd.shader_id = pDevice->PipelineState.VertexShader;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_SET_SHADER, &cmd, sizeof(cmd));
}

void APIENTRY PvgpuPsSetShader(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HSHADER hShader)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_SHADER* pShader;
    PvgpuCmdSetShader cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pShader = (PVGPU_UMD_SHADER*)hShader.pDrvPrivate;
    
    /* Track in device state */
    pDevice->PipelineState.PixelShader = pShader ? pShader->HostHandle : 0;
    
    /* Build command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_SET_SHADER;
    cmd.header.command_size = sizeof(cmd);
    cmd.stage = PVGPU_STAGE_PIXEL;
    cmd.shader_id = pDevice->PipelineState.PixelShader;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_SET_SHADER, &cmd, sizeof(cmd));
}

void APIENTRY PvgpuGsSetShader(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HSHADER hShader)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_SHADER* pShader;
    PvgpuCmdSetShader cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pShader = (PVGPU_UMD_SHADER*)hShader.pDrvPrivate;
    
    /* Track in device state */
    pDevice->PipelineState.GeometryShader = pShader ? pShader->HostHandle : 0;
    
    /* Build command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_SET_SHADER;
    cmd.header.command_size = sizeof(cmd);
    cmd.stage = PVGPU_STAGE_GEOMETRY;
    cmd.shader_id = pDevice->PipelineState.GeometryShader;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_SET_SHADER, &cmd, sizeof(cmd));
}

void APIENTRY PvgpuSetRenderTargets(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_reads_(NumViews) CONST D3D10DDI_HRENDERTARGETVIEW* phRenderTargetView,
    _In_ UINT NumViews,
    _In_ UINT ClearSlots,
    _In_ D3D10DDI_HDEPTHSTENCILVIEW hDepthStencilView)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_RESOURCE* pDSV;
    PvgpuCmdSetRenderTarget cmd;
    UINT i;
    
    UNREFERENCED_PARAMETER(ClearSlots);
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pDSV = (PVGPU_UMD_RESOURCE*)hDepthStencilView.pDrvPrivate;
    
    /* Limit to maximum supported */
    if (NumViews > 8) NumViews = 8;
    
    /* Build command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_SET_RENDER_TARGET;
    cmd.header.command_size = sizeof(cmd);
    cmd.num_rtvs = NumViews;
    cmd.dsv_id = pDSV ? pDSV->HostHandle : 0;
    
    for (i = 0; i < NumViews; i++)
    {
        PVGPU_UMD_RESOURCE* pRTV = (PVGPU_UMD_RESOURCE*)phRenderTargetView[i].pDrvPrivate;
        cmd.rtv_ids[i] = pRTV ? pRTV->HostHandle : 0;
        
        /* Track in device state */
        pDevice->PipelineState.RenderTargets[i] = cmd.rtv_ids[i];
    }
    
    pDevice->PipelineState.RenderTargetCount = NumViews;
    pDevice->PipelineState.DepthStencilView = cmd.dsv_id;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_SET_RENDER_TARGET, &cmd, sizeof(cmd));
}

void APIENTRY PvgpuSetViewports(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT NumViewports,
    _In_ UINT ClearViewports,
    _In_reads_(NumViewports) CONST D3D10_DDI_VIEWPORT* pViewports)
{
    PVGPU_UMD_DEVICE* pDevice;
    PvgpuCmdSetViewport cmd;
    UINT i;
    
    UNREFERENCED_PARAMETER(ClearViewports);
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    
    /* Limit to maximum supported */
    if (NumViewports > 16) NumViewports = 16;
    
    /* Build command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_SET_VIEWPORT;
    cmd.header.command_size = sizeof(cmd);
    cmd.num_viewports = NumViewports;
    
    for (i = 0; i < NumViewports; i++)
    {
        cmd.viewports[i].x = pViewports[i].TopLeftX;
        cmd.viewports[i].y = pViewports[i].TopLeftY;
        cmd.viewports[i].width = pViewports[i].Width;
        cmd.viewports[i].height = pViewports[i].Height;
        cmd.viewports[i].min_depth = pViewports[i].MinDepth;
        cmd.viewports[i].max_depth = pViewports[i].MaxDepth;
    }
    
    pDevice->PipelineState.ViewportCount = NumViewports;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_SET_VIEWPORT, &cmd, sizeof(cmd));
}

void APIENTRY PvgpuSetScissorRects(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT NumRects,
    _In_ UINT ClearRects,
    _In_reads_(NumRects) CONST D3D10_DDI_RECT* pRects)
{
    PVGPU_UMD_DEVICE* pDevice;
    PvgpuCmdSetScissor cmd;
    UINT i;
    
    UNREFERENCED_PARAMETER(ClearRects);
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    
    /* Limit to maximum supported */
    if (NumRects > 16) NumRects = 16;
    
    /* Build command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_SET_SCISSOR;
    cmd.header.command_size = sizeof(cmd);
    cmd.num_rects = NumRects;
    
    for (i = 0; i < NumRects; i++)
    {
        cmd.rects[i].left = pRects[i].left;
        cmd.rects[i].top = pRects[i].top;
        cmd.rects[i].right = pRects[i].right;
        cmd.rects[i].bottom = pRects[i].bottom;
    }
    
    pDevice->PipelineState.ScissorRectCount = NumRects;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_SET_SCISSOR, &cmd, sizeof(cmd));
}

void APIENTRY PvgpuSetBlendState(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HBLENDSTATE hBlendState,
    _In_reads_(4) CONST FLOAT BlendFactor[4],
    _In_ UINT SampleMask)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_RESOURCE* pBlendState;
    PvgpuCmdSetBlendState cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pBlendState = (PVGPU_UMD_RESOURCE*)hBlendState.pDrvPrivate;
    
    /* Track in device state */
    pDevice->PipelineState.BlendState = pBlendState ? pBlendState->HostHandle : 0;
    pDevice->PipelineState.BlendFactor[0] = BlendFactor[0];
    pDevice->PipelineState.BlendFactor[1] = BlendFactor[1];
    pDevice->PipelineState.BlendFactor[2] = BlendFactor[2];
    pDevice->PipelineState.BlendFactor[3] = BlendFactor[3];
    pDevice->PipelineState.SampleMask = SampleMask;
    
    /* Build command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_SET_BLEND_STATE;
    cmd.header.command_size = sizeof(cmd);
    cmd.blend_state_id = pDevice->PipelineState.BlendState;
    cmd.blend_factor[0] = BlendFactor[0];
    cmd.blend_factor[1] = BlendFactor[1];
    cmd.blend_factor[2] = BlendFactor[2];
    cmd.blend_factor[3] = BlendFactor[3];
    cmd.sample_mask = SampleMask;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_SET_BLEND_STATE, &cmd, sizeof(cmd));
}

void APIENTRY PvgpuSetDepthStencilState(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HDEPTHSTENCILSTATE hDepthStencilState,
    _In_ UINT StencilRef)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_RESOURCE* pDSState;
    PvgpuCmdSetDepthStencilState cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pDSState = (PVGPU_UMD_RESOURCE*)hDepthStencilState.pDrvPrivate;
    
    /* Track in device state */
    pDevice->PipelineState.DepthStencilState = pDSState ? pDSState->HostHandle : 0;
    pDevice->PipelineState.StencilRef = StencilRef;
    
    /* Build command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_SET_DEPTH_STENCIL;
    cmd.header.command_size = sizeof(cmd);
    cmd.depth_stencil_state_id = pDevice->PipelineState.DepthStencilState;
    cmd.stencil_ref = StencilRef;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_SET_DEPTH_STENCIL, &cmd, sizeof(cmd));
}

void APIENTRY PvgpuSetRasterizerState(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HRASTERIZERSTATE hRasterizerState)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_RESOURCE* pRSState;
    PvgpuCmdSetRasterizerState cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pRSState = (PVGPU_UMD_RESOURCE*)hRasterizerState.pDrvPrivate;
    
    /* Track in device state */
    pDevice->PipelineState.RasterizerState = pRSState ? pRSState->HostHandle : 0;
    
    /* Build command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_SET_RASTERIZER_STATE;
    cmd.header.command_size = sizeof(cmd);
    cmd.rasterizer_state_id = pDevice->PipelineState.RasterizerState;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_SET_RASTERIZER_STATE, &cmd, sizeof(cmd));
}

/* ============================================================================
 * Resource Operations
 * ============================================================================ */

void APIENTRY PvgpuResourceCopy(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HRESOURCE hDstResource,
    _In_ D3D10DDI_HRESOURCE hSrcResource)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_RESOURCE* pDst;
    PVGPU_UMD_RESOURCE* pSrc;
    PvgpuCmdCopyResource cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pDst = (PVGPU_UMD_RESOURCE*)hDstResource.pDrvPrivate;
    pSrc = (PVGPU_UMD_RESOURCE*)hSrcResource.pDrvPrivate;
    
    if (pDst == NULL || pSrc == NULL)
    {
        return;
    }
    
    /* Build command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_COPY_RESOURCE;
    cmd.header.command_size = sizeof(cmd);
    cmd.dst_resource_id = pDst->HostHandle;
    cmd.src_resource_id = pSrc->HostHandle;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_COPY_RESOURCE, &cmd, sizeof(cmd));
}

void APIENTRY PvgpuResourceCopyRegion(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HRESOURCE hDstResource,
    _In_ UINT DstSubresource,
    _In_ UINT DstX,
    _In_ UINT DstY,
    _In_ UINT DstZ,
    _In_ D3D10DDI_HRESOURCE hSrcResource,
    _In_ UINT SrcSubresource,
    _In_opt_ CONST D3D10_DDI_BOX* pSrcBox)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_RESOURCE* pDst;
    PVGPU_UMD_RESOURCE* pSrc;
    PvgpuCmdCopyResourceRegion cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pDst = (PVGPU_UMD_RESOURCE*)hDstResource.pDrvPrivate;
    pSrc = (PVGPU_UMD_RESOURCE*)hSrcResource.pDrvPrivate;
    
    if (pDst == NULL || pSrc == NULL)
    {
        return;
    }
    
    /* Build command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_COPY_RESOURCE;
    cmd.header.command_size = sizeof(cmd);
    cmd.dst_resource_id = pDst->HostHandle;
    cmd.dst_subresource = DstSubresource;
    cmd.dst_x = DstX;
    cmd.dst_y = DstY;
    cmd.dst_z = DstZ;
    cmd.src_resource_id = pSrc->HostHandle;
    cmd.src_subresource = SrcSubresource;
    
    /* Copy source box if provided */
    if (pSrcBox != NULL)
    {
        cmd.has_src_box = 1;
        cmd.src_box.left = pSrcBox->left;
        cmd.src_box.top = pSrcBox->top;
        cmd.src_box.front = pSrcBox->front;
        cmd.src_box.right = pSrcBox->right;
        cmd.src_box.bottom = pSrcBox->bottom;
        cmd.src_box.back = pSrcBox->back;
    }
    else
    {
        cmd.has_src_box = 0;
    }
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_COPY_RESOURCE, &cmd, sizeof(cmd));
}

void APIENTRY PvgpuResourceUpdateSubresourceUP(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HRESOURCE hDstResource,
    _In_ UINT DstSubresource,
    _In_opt_ CONST D3D10_DDI_BOX* pDstBox,
    _In_ CONST VOID* pSysMemUP,
    _In_ UINT RowPitch,
    _In_ UINT DepthPitch)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_RESOURCE* pDst;
    PvgpuCmdUpdateResource cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pDst = (PVGPU_UMD_RESOURCE*)hDstResource.pDrvPrivate;
    
    if (pDst == NULL || pSysMemUP == NULL)
    {
        return;
    }
    
    /* Build command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_UPDATE_RESOURCE;
    cmd.header.command_size = sizeof(cmd);
    cmd.header.resource_id = pDst->HostHandle;
    cmd.subresource = DstSubresource;
    cmd.row_pitch = RowPitch;
    cmd.depth_pitch = DepthPitch;
    
    /* Set destination region if box is provided */
    if (pDstBox != NULL)
    {
        cmd.dst_x = pDstBox->left;
        cmd.dst_y = pDstBox->top;
        cmd.dst_z = pDstBox->front;
        cmd.width = pDstBox->right - pDstBox->left;
        cmd.height = pDstBox->bottom - pDstBox->top;
        cmd.depth = pDstBox->back - pDstBox->front;
    }
    else
    {
        /* Full resource update */
        cmd.dst_x = 0;
        cmd.dst_y = 0;
        cmd.dst_z = 0;
        cmd.width = pDst->Width;
        cmd.height = pDst->Height;
        cmd.depth = pDst->Depth > 0 ? pDst->Depth : 1;
    }
    
    /*
     * NOTE: The actual data copy to shared memory would happen here.
     * For now, we set heap_offset to 0 as a placeholder.
     * In a full implementation:
     * 1. Allocate space in the resource heap
     * 2. Copy pSysMemUP data to heap at that offset
     * 3. Set cmd.heap_offset to that location
     */
    cmd.heap_offset = 0; /* TODO: Implement shared memory data transfer */
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_UPDATE_RESOURCE, &cmd, sizeof(cmd));
    
    PVGPU_TRACE("UpdateSubresourceUP: resource %u subres %u",
        pDst->HostHandle, DstSubresource);
}

void APIENTRY PvgpuResourceMap(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HRESOURCE hResource,
    _In_ UINT Subresource,
    _In_ D3D10_DDI_MAP MapType,
    _In_ UINT MapFlags,
    _Out_ D3D10DDI_MAPPED_SUBRESOURCE* pMappedSubresource)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_RESOURCE* pResource;
    PvgpuCmdMapResource cmd;
    
    UNREFERENCED_PARAMETER(MapFlags);
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pResource = (PVGPU_UMD_RESOURCE*)hResource.pDrvPrivate;
    
    if (pResource == NULL || pMappedSubresource == NULL)
    {
        return;
    }
    
    /*
     * Resource mapping in a paravirtualized driver requires:
     * 1. Allocating space in shared memory heap
     * 2. For read maps: Submitting command to host to copy data to heap
     * 3. Returning the host-visible shared memory pointer to the app
     * 4. On unmap: For write maps, submit command to copy heap to resource
     * 
     * For now, we implement a basic version that tracks the mapping
     * but requires shared memory infrastructure to be fully functional.
     */
    
    /* Build map command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_MAP_RESOURCE;
    cmd.header.command_size = sizeof(cmd);
    cmd.header.resource_id = pResource->HostHandle;
    cmd.subresource = Subresource;
    cmd.map_type = MapType;
    cmd.heap_offset = 0; /* TODO: Allocate from shared memory heap */
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_MAP_RESOURCE, &cmd, sizeof(cmd));
    
    /* Mark resource as mapped */
    pResource->IsMapped = TRUE;
    
    /*
     * TODO: Return actual mapped pointer from shared memory.
     * For now, return NULL which will cause apps to fail gracefully.
     * In full implementation:
     * - pMappedSubresource->pData = sharedMemoryBase + heapOffset
     * - pMappedSubresource->RowPitch = calculated based on format/width
     * - pMappedSubresource->DepthPitch = calculated for 3D textures
     */
    pMappedSubresource->pData = NULL;
    pMappedSubresource->RowPitch = 0;
    pMappedSubresource->DepthPitch = 0;
    
    PVGPU_TRACE("ResourceMap: resource %u subres %u type %d (NOT FULLY IMPLEMENTED)",
        pResource->HostHandle, Subresource, MapType);
}

void APIENTRY PvgpuResourceUnmap(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HRESOURCE hResource,
    _In_ UINT Subresource)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_RESOURCE* pResource;
    PvgpuCommandHeader cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pResource = (PVGPU_UMD_RESOURCE*)hResource.pDrvPrivate;
    
    if (pResource == NULL || !pResource->IsMapped)
    {
        return;
    }
    
    /* Build unmap command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.command_type = PVGPU_CMD_UNMAP_RESOURCE;
    cmd.command_size = sizeof(cmd);
    cmd.resource_id = pResource->HostHandle;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_UNMAP_RESOURCE, &cmd, sizeof(cmd));
    
    /* Mark resource as unmapped */
    pResource->IsMapped = FALSE;
    pResource->MappedAddress = NULL;
    pResource->MappedSize = 0;
    
    PVGPU_TRACE("ResourceUnmap: resource %u subres %u", pResource->HostHandle, Subresource);
}

/* ============================================================================
 * Present
 * ============================================================================ */

void APIENTRY PvgpuPresent(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ DXGI_DDI_ARG_PRESENT* pPresentData)
{
    PVGPU_UMD_DEVICE* pDevice;
    PvgpuCmdPresent cmd;
    
    UNREFERENCED_PARAMETER(pPresentData);
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    
    /* Flush pending commands first */
    PvgpuFlushCommandBuffer(pDevice);
    
    /* Submit present command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.swapchain_id = 0; /* TODO: Get from present data */
    cmd.sync_interval = 1;
    cmd.flags = 0;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_PRESENT, &cmd, sizeof(cmd));
    
    /* Flush again to ensure present is processed */
    PvgpuFlushCommandBuffer(pDevice);
}

void APIENTRY PvgpuBlt(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ DXGI_DDI_ARG_BLT* pBltData)
{
    UNREFERENCED_PARAMETER(hDevice);
    UNREFERENCED_PARAMETER(pBltData);
    /* TODO: Implement blit operation */
}

/* ============================================================================
 * Command Buffer Helpers
 * ============================================================================ */

BOOL PvgpuWriteCommand(
    _In_ PVGPU_UMD_DEVICE* pDevice,
    _In_ UINT32 CommandType,
    _In_ const void* pPayload,
    _In_ SIZE_T PayloadSize)
{
    PvgpuCommand* pCmd;
    SIZE_T totalSize;
    
    if (pDevice == NULL || pDevice->pCommandBuffer == NULL)
    {
        return FALSE;
    }
    
    totalSize = sizeof(PvgpuCommand) + PayloadSize;
    
    /* Check if we need to flush */
    if (pDevice->CommandBufferOffset + totalSize > pDevice->CommandBufferSize)
    {
        PvgpuFlushCommandBuffer(pDevice);
    }
    
    /* Write command header */
    pCmd = (PvgpuCommand*)(pDevice->pCommandBuffer + pDevice->CommandBufferOffset);
    pCmd->type = CommandType;
    pCmd->size = (UINT32)PayloadSize;
    pCmd->flags = 0;
    pCmd->fence_id = 0;
    
    /* Copy payload */
    if (pPayload != NULL && PayloadSize > 0)
    {
        CopyMemory(pCmd + 1, pPayload, PayloadSize);
    }
    
    pDevice->CommandBufferOffset += totalSize;
    pDevice->CommandsSubmitted++;
    
    return TRUE;
}

void PvgpuFlushCommandBuffer(
    _In_ PVGPU_UMD_DEVICE* pDevice)
{
    if (pDevice == NULL || pDevice->CommandBufferOffset == 0)
    {
        return;
    }
    
    /* Submit command buffer to KMD via runtime callback */
    if (pDevice->pKTCallbacks != NULL && pDevice->pKTCallbacks->pfnRenderCb != NULL)
    {
        D3DDDICB_RENDER renderCb;
        
        ZeroMemory(&renderCb, sizeof(renderCb));
        renderCb.CommandLength = (UINT)pDevice->CommandBufferOffset;
        renderCb.NumAllocations = 0;
        renderCb.NumPatchLocations = 0;
        
        pDevice->pKTCallbacks->pfnRenderCb(pDevice->hRTDevice.handle, &renderCb);
    }
    
    /* Reset buffer offset */
    pDevice->CommandBufferOffset = 0;
}

UINT32 PvgpuAllocateResourceHandle(
    _In_ PVGPU_UMD_DEVICE* pDevice)
{
    UINT32 handle;
    
    EnterCriticalSection(&pDevice->ResourceLock);
    handle = pDevice->NextResourceHandle++;
    LeaveCriticalSection(&pDevice->ResourceLock);
    
    return handle;
}

PVGPU_UMD_RESOURCE* PvgpuGetResource(
    _In_ PVGPU_UMD_DEVICE* pDevice,
    _In_ D3D10DDI_HRESOURCE hResource)
{
    UNREFERENCED_PARAMETER(pDevice);
    
    if (hResource.pDrvPrivate == NULL)
    {
        return NULL;
    }
    
    return (PVGPU_UMD_RESOURCE*)hResource.pDrvPrivate;
}
