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
#include <d3dkmthk.h>

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
pAdapter->SupportsCompute = TRUE;
    pAdapter->SupportsTessellation = TRUE;
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
    HRESULT hr;
    
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
    
    /* Initialize ring buffer lock */
    InitializeCriticalSection(&pDevice->RingLock);
    
    /* Allocate staging buffer for command batching */
    pDevice->StagingBufferSize = PVGPU_UMD_COMMAND_BUFFER_SIZE;
    pDevice->pStagingBuffer = (UINT8*)HeapAlloc(
        GetProcessHeap(),
        HEAP_ZERO_MEMORY,
        pDevice->StagingBufferSize);
    
    if (pDevice->pStagingBuffer == NULL)
    {
        DeleteCriticalSection(&pDevice->RingLock);
        DeleteCriticalSection(&pDevice->ResourceLock);
        HeapFree(GetProcessHeap(), 0, pDevice->pResources);
        return E_OUTOFMEMORY;
    }
    
    pDevice->StagingOffset = 0;
    pDevice->NextFenceValue = 1;
    pDevice->LastFenceSubmitted = 0;
    pDevice->LastPresentFence = 0;
    
    /* Get shared memory info from KMD via escape */
    hr = PvgpuInitSharedMemory(pDevice);
    if (FAILED(hr))
    {
        PVGPU_TRACE("PvgpuCreateDevice: Failed to init shared memory, hr=0x%08X", hr);
        /* Continue anyway - will work without direct shmem access */
        /* Commands will be staged but doorbell won't work */
    }
    
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
    pDeviceFuncs->pfnCreateHullShader = PvgpuCreateHullShader;
    pDeviceFuncs->pfnCreateDomainShader = PvgpuCreateDomainShader;
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
    pDeviceFuncs->pfnHsSetShader = PvgpuHsSetShader;
    pDeviceFuncs->pfnDsSetShader = PvgpuDsSetShader;
    
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
    
    /* State object creation */
    pDeviceFuncs->pfnCalcPrivateBlendStateSize = PvgpuCalcPrivateBlendStateSize;
    pDeviceFuncs->pfnCreateBlendState = PvgpuCreateBlendState;
    pDeviceFuncs->pfnDestroyBlendState = PvgpuDestroyBlendState;
    pDeviceFuncs->pfnCalcPrivateRasterizerStateSize = PvgpuCalcPrivateRasterizerStateSize;
    pDeviceFuncs->pfnCreateRasterizerState = PvgpuCreateRasterizerState;
    pDeviceFuncs->pfnDestroyRasterizerState = PvgpuDestroyRasterizerState;
    pDeviceFuncs->pfnCalcPrivateDepthStencilStateSize = PvgpuCalcPrivateDepthStencilStateSize;
    pDeviceFuncs->pfnCreateDepthStencilState = PvgpuCreateDepthStencilState;
    pDeviceFuncs->pfnDestroyDepthStencilState = PvgpuDestroyDepthStencilState;
    pDeviceFuncs->pfnCalcPrivateSamplerSize = PvgpuCalcPrivateSamplerSize;
    pDeviceFuncs->pfnCreateSampler = PvgpuCreateSampler;
    pDeviceFuncs->pfnDestroySampler = PvgpuDestroySampler;
    pDeviceFuncs->pfnCalcPrivateElementLayoutSize = PvgpuCalcPrivateElementLayoutSize;
    pDeviceFuncs->pfnCreateElementLayout = PvgpuCreateElementLayout;
    pDeviceFuncs->pfnDestroyElementLayout = PvgpuDestroyElementLayout;
    
    /* View creation */
    pDeviceFuncs->pfnCalcPrivateRenderTargetViewSize = PvgpuCalcPrivateRenderTargetViewSize;
    pDeviceFuncs->pfnCreateRenderTargetView = PvgpuCreateRenderTargetView;
    pDeviceFuncs->pfnDestroyRenderTargetView = PvgpuDestroyRenderTargetView;
    pDeviceFuncs->pfnCalcPrivateDepthStencilViewSize = PvgpuCalcPrivateDepthStencilViewSize;
    pDeviceFuncs->pfnCreateDepthStencilView = PvgpuCreateDepthStencilView;
    pDeviceFuncs->pfnDestroyDepthStencilView = PvgpuDestroyDepthStencilView;
    pDeviceFuncs->pfnCalcPrivateShaderResourceViewSize = PvgpuCalcPrivateShaderResourceViewSize;
    pDeviceFuncs->pfnCreateShaderResourceView = PvgpuCreateShaderResourceView;
    pDeviceFuncs->pfnDestroyShaderResourceView = PvgpuDestroyShaderResourceView;
    
    /* Constant buffers and shader resources */
    pDeviceFuncs->pfnVsSetConstantBuffers = PvgpuVsSetConstantBuffers;
    pDeviceFuncs->pfnPsSetConstantBuffers = PvgpuPsSetConstantBuffers;
    pDeviceFuncs->pfnGsSetConstantBuffers = PvgpuGsSetConstantBuffers;
    pDeviceFuncs->pfnHsSetConstantBuffers = PvgpuHsSetConstantBuffers;
    pDeviceFuncs->pfnDsSetConstantBuffers = PvgpuDsSetConstantBuffers;
    pDeviceFuncs->pfnVsSetShaderResources = PvgpuVsSetShaderResources;
    pDeviceFuncs->pfnPsSetShaderResources = PvgpuPsSetShaderResources;
    pDeviceFuncs->pfnGsSetShaderResources = PvgpuGsSetShaderResources;
    pDeviceFuncs->pfnHsSetShaderResources = PvgpuHsSetShaderResources;
    pDeviceFuncs->pfnDsSetShaderResources = PvgpuDsSetShaderResources;
    pDeviceFuncs->pfnVsSetSamplers = PvgpuVsSetSamplers;
    pDeviceFuncs->pfnPsSetSamplers = PvgpuPsSetSamplers;
    pDeviceFuncs->pfnGsSetSamplers = PvgpuGsSetSamplers;
    pDeviceFuncs->pfnHsSetSamplers = PvgpuHsSetSamplers;
    pDeviceFuncs->pfnDsSetSamplers = PvgpuDsSetSamplers;
    
    /* Compute shader stage */
    pDeviceFuncs->pfnCsSetShader = PvgpuCsSetShader;
    pDeviceFuncs->pfnCsSetConstantBuffers = PvgpuCsSetConstantBuffers;
    pDeviceFuncs->pfnCsSetShaderResources = PvgpuCsSetShaderResources;
    pDeviceFuncs->pfnCsSetSamplers = PvgpuCsSetSamplers;
    pDeviceFuncs->pfnCsSetUnorderedAccessViews = PvgpuCsSetUnorderedAccessViews;
    pDeviceFuncs->pfnDispatch = PvgpuDispatch;
    pDeviceFuncs->pfnDispatchIndirect = PvgpuDispatchIndirect;
    
    /* UAV creation */
    pDeviceFuncs->pfnCalcPrivateUnorderedAccessViewSize = PvgpuCalcPrivateUnorderedAccessViewSize;
    pDeviceFuncs->pfnCreateUnorderedAccessView = PvgpuCreateUnorderedAccessView;
    pDeviceFuncs->pfnDestroyUnorderedAccessView = PvgpuDestroyUnorderedAccessView;
    
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

/*
 * Format support table for D3D11 feature-level 11.0.
 * Since all rendering is forwarded to the host GPU via D3D11, we report
 * comprehensive format support. The host backend validates actual hardware
 * support at resource creation time.
 *
 * Support flags:
 *   0x01 = D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE
 *   0x02 = D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET
 *   0x04 = D3D10_DDI_FORMAT_SUPPORT_BLENDABLE
 *   0x08 = D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET
 *   0x10 = D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD
 */
#define PVGPU_FMT_SAMPLE      0x01
#define PVGPU_FMT_RT          0x02
#define PVGPU_FMT_BLEND       0x04
#define PVGPU_FMT_MSRT        0x08
#define PVGPU_FMT_MSLOAD      0x10

#define PVGPU_FMT_ALL   (PVGPU_FMT_SAMPLE | PVGPU_FMT_RT | PVGPU_FMT_BLEND | PVGPU_FMT_MSRT | PVGPU_FMT_MSLOAD)
#define PVGPU_FMT_RT_FULL (PVGPU_FMT_SAMPLE | PVGPU_FMT_RT | PVGPU_FMT_BLEND)
#define PVGPU_FMT_DS     (PVGPU_FMT_SAMPLE) /* Depth-stencil: sample only */

typedef struct _PVGPU_FORMAT_ENTRY {
    DXGI_FORMAT Format;
    UINT        Support;
} PVGPU_FORMAT_ENTRY;

static const PVGPU_FORMAT_ENTRY g_PvgpuFormatTable[] = {
    /* Standard RGBA formats */
    { DXGI_FORMAT_R32G32B32A32_FLOAT,       PVGPU_FMT_ALL },
    { DXGI_FORMAT_R32G32B32A32_UINT,        PVGPU_FMT_SAMPLE | PVGPU_FMT_RT },
    { DXGI_FORMAT_R32G32B32A32_SINT,        PVGPU_FMT_SAMPLE | PVGPU_FMT_RT },
    { DXGI_FORMAT_R16G16B16A16_FLOAT,       PVGPU_FMT_ALL },
    { DXGI_FORMAT_R16G16B16A16_UNORM,       PVGPU_FMT_ALL },
    { DXGI_FORMAT_R16G16B16A16_UINT,        PVGPU_FMT_SAMPLE | PVGPU_FMT_RT },
    { DXGI_FORMAT_R16G16B16A16_SNORM,       PVGPU_FMT_RT_FULL },
    { DXGI_FORMAT_R16G16B16A16_SINT,        PVGPU_FMT_SAMPLE | PVGPU_FMT_RT },
    { DXGI_FORMAT_R32G32_FLOAT,             PVGPU_FMT_RT_FULL },
    { DXGI_FORMAT_R32G32_UINT,              PVGPU_FMT_SAMPLE | PVGPU_FMT_RT },
    { DXGI_FORMAT_R32G32_SINT,              PVGPU_FMT_SAMPLE | PVGPU_FMT_RT },
    { DXGI_FORMAT_R10G10B10A2_UNORM,        PVGPU_FMT_ALL },
    { DXGI_FORMAT_R10G10B10A2_UINT,         PVGPU_FMT_SAMPLE | PVGPU_FMT_RT },
    { DXGI_FORMAT_R11G11B10_FLOAT,          PVGPU_FMT_RT_FULL },
    { DXGI_FORMAT_R8G8B8A8_UNORM,           PVGPU_FMT_ALL },
    { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,      PVGPU_FMT_ALL },
    { DXGI_FORMAT_R8G8B8A8_UINT,            PVGPU_FMT_SAMPLE | PVGPU_FMT_RT },
    { DXGI_FORMAT_R8G8B8A8_SNORM,           PVGPU_FMT_RT_FULL },
    { DXGI_FORMAT_R8G8B8A8_SINT,            PVGPU_FMT_SAMPLE | PVGPU_FMT_RT },
    /* RG formats */
    { DXGI_FORMAT_R16G16_FLOAT,             PVGPU_FMT_ALL },
    { DXGI_FORMAT_R16G16_UNORM,             PVGPU_FMT_ALL },
    { DXGI_FORMAT_R16G16_UINT,              PVGPU_FMT_SAMPLE | PVGPU_FMT_RT },
    { DXGI_FORMAT_R16G16_SNORM,             PVGPU_FMT_RT_FULL },
    { DXGI_FORMAT_R16G16_SINT,              PVGPU_FMT_SAMPLE | PVGPU_FMT_RT },
    { DXGI_FORMAT_R32_FLOAT,                PVGPU_FMT_ALL },
    { DXGI_FORMAT_R32_UINT,                 PVGPU_FMT_SAMPLE | PVGPU_FMT_RT },
    { DXGI_FORMAT_R32_SINT,                 PVGPU_FMT_SAMPLE | PVGPU_FMT_RT },
    { DXGI_FORMAT_R8G8_UNORM,               PVGPU_FMT_ALL },
    { DXGI_FORMAT_R8G8_UINT,                PVGPU_FMT_SAMPLE | PVGPU_FMT_RT },
    { DXGI_FORMAT_R8G8_SNORM,               PVGPU_FMT_RT_FULL },
    { DXGI_FORMAT_R8G8_SINT,                PVGPU_FMT_SAMPLE | PVGPU_FMT_RT },
    /* Single-channel formats */
    { DXGI_FORMAT_R16_FLOAT,                PVGPU_FMT_ALL },
    { DXGI_FORMAT_R16_UNORM,                PVGPU_FMT_ALL },
    { DXGI_FORMAT_R16_UINT,                 PVGPU_FMT_SAMPLE | PVGPU_FMT_RT },
    { DXGI_FORMAT_R16_SNORM,                PVGPU_FMT_RT_FULL },
    { DXGI_FORMAT_R16_SINT,                 PVGPU_FMT_SAMPLE | PVGPU_FMT_RT },
    { DXGI_FORMAT_R8_UNORM,                 PVGPU_FMT_ALL },
    { DXGI_FORMAT_R8_UINT,                  PVGPU_FMT_SAMPLE | PVGPU_FMT_RT },
    { DXGI_FORMAT_R8_SNORM,                 PVGPU_FMT_RT_FULL },
    { DXGI_FORMAT_R8_SINT,                  PVGPU_FMT_SAMPLE | PVGPU_FMT_RT },
    { DXGI_FORMAT_A8_UNORM,                 PVGPU_FMT_RT_FULL },
    /* Depth-stencil formats */
    { DXGI_FORMAT_D32_FLOAT,                PVGPU_FMT_DS },
    { DXGI_FORMAT_D24_UNORM_S8_UINT,        PVGPU_FMT_DS },
    { DXGI_FORMAT_D16_UNORM,                PVGPU_FMT_DS },
    { DXGI_FORMAT_D32_FLOAT_S8X24_UINT,     PVGPU_FMT_DS },
    /* Typeless depth formats (for SRV binding of depth textures) */
    { DXGI_FORMAT_R32_TYPELESS,             PVGPU_FMT_SAMPLE },
    { DXGI_FORMAT_R24G8_TYPELESS,           PVGPU_FMT_SAMPLE },
    { DXGI_FORMAT_R16_TYPELESS,             PVGPU_FMT_SAMPLE },
    { DXGI_FORMAT_R32G8X24_TYPELESS,        PVGPU_FMT_SAMPLE },
    { DXGI_FORMAT_R24_UNORM_X8_TYPELESS,    PVGPU_FMT_SAMPLE },
    { DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, PVGPU_FMT_SAMPLE },
    /* Compressed formats (BC) */
    { DXGI_FORMAT_BC1_UNORM,                PVGPU_FMT_SAMPLE },
    { DXGI_FORMAT_BC1_UNORM_SRGB,           PVGPU_FMT_SAMPLE },
    { DXGI_FORMAT_BC2_UNORM,                PVGPU_FMT_SAMPLE },
    { DXGI_FORMAT_BC2_UNORM_SRGB,           PVGPU_FMT_SAMPLE },
    { DXGI_FORMAT_BC3_UNORM,                PVGPU_FMT_SAMPLE },
    { DXGI_FORMAT_BC3_UNORM_SRGB,           PVGPU_FMT_SAMPLE },
    { DXGI_FORMAT_BC4_UNORM,                PVGPU_FMT_SAMPLE },
    { DXGI_FORMAT_BC4_SNORM,                PVGPU_FMT_SAMPLE },
    { DXGI_FORMAT_BC5_UNORM,                PVGPU_FMT_SAMPLE },
    { DXGI_FORMAT_BC5_SNORM,                PVGPU_FMT_SAMPLE },
    { DXGI_FORMAT_BC6H_UF16,               PVGPU_FMT_SAMPLE },
    { DXGI_FORMAT_BC6H_SF16,               PVGPU_FMT_SAMPLE },
    { DXGI_FORMAT_BC7_UNORM,                PVGPU_FMT_SAMPLE },
    { DXGI_FORMAT_BC7_UNORM_SRGB,           PVGPU_FMT_SAMPLE },
    /* BGRA formats (common for swap chains and UI) */
    { DXGI_FORMAT_B8G8R8A8_UNORM,           PVGPU_FMT_ALL },
    { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,      PVGPU_FMT_ALL },
    { DXGI_FORMAT_B8G8R8X8_UNORM,           PVGPU_FMT_ALL },
    { DXGI_FORMAT_B8G8R8X8_UNORM_SRGB,      PVGPU_FMT_ALL },
    { DXGI_FORMAT_B5G6R5_UNORM,             PVGPU_FMT_RT_FULL },
    { DXGI_FORMAT_B5G5R5A1_UNORM,           PVGPU_FMT_RT_FULL },
    { DXGI_FORMAT_B4G4R4A4_UNORM,           PVGPU_FMT_SAMPLE },
    /* RGB32 3-component (vertex buffer only) */
    { DXGI_FORMAT_R32G32B32_FLOAT,          PVGPU_FMT_SAMPLE },
    { DXGI_FORMAT_R32G32B32_UINT,           PVGPU_FMT_SAMPLE },
    { DXGI_FORMAT_R32G32B32_SINT,           PVGPU_FMT_SAMPLE },
    /* Special formats */
    { DXGI_FORMAT_R9G9B9E5_SHAREDEXP,       PVGPU_FMT_SAMPLE },
    { DXGI_FORMAT_R1_UNORM,                 PVGPU_FMT_SAMPLE },
};

#define PVGPU_FORMAT_TABLE_SIZE (sizeof(g_PvgpuFormatTable) / sizeof(g_PvgpuFormatTable[0]))

static void PvgpuFillFormatSupportData(
    _Out_writes_(maxEntries) DXGI_FORMAT_SUPPORT_DATA* pFormatData,
    _In_ UINT maxEntries)
{
    UINT i;
    UINT count = (UINT)PVGPU_FORMAT_TABLE_SIZE;
    
    if (count > maxEntries)
        count = maxEntries;
    
    for (i = 0; i < count; i++)
    {
        pFormatData[i].Format = g_PvgpuFormatTable[i].Format;
        pFormatData[i].Support = g_PvgpuFormatTable[i].Support;
    }
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
            *(UINT*)pData->pData = PVGPU_FORMAT_TABLE_SIZE;
        }
        break;
        
    case D3D10_2DDICAPS_TYPE_GETFORMATDATA:
        /*
         * Fill in format support data. Since we forward all rendering to the host
         * GPU via D3D11, we report comprehensive format support matching a typical
         * D3D11 feature-level 11.0 device. The host backend will validate actual
         * hardware support at resource creation time.
         */
        if (pData->pData != NULL)
        {
            PvgpuFillFormatSupportData(
                (DXGI_FORMAT_SUPPORT_DATA*)pData->pData,
                pData->DataSize / sizeof(DXGI_FORMAT_SUPPORT_DATA));
        }
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
        
        /* Free staging buffer */
        if (pDevice->pStagingBuffer != NULL)
        {
            HeapFree(GetProcessHeap(), 0, pDevice->pStagingBuffer);
        }
        
        /* Free resource tracking */
        if (pDevice->pResources != NULL)
        {
            HeapFree(GetProcessHeap(), 0, pDevice->pResources);
        }
        
        DeleteCriticalSection(&pDevice->ResourceLock);
        DeleteCriticalSection(&pDevice->RingLock);
        
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
    cmd.header.command_type = PVGPU_CMD_CREATE_RESOURCE;
    cmd.header.command_size = sizeof(cmd);
    cmd.header.resource_id = pResource->HostHandle;
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
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_DESTROY_RESOURCE;
    cmd.header.command_size = sizeof(cmd);
    cmd.header.resource_id = pResource->HostHandle;
    
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
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_RESOURCE* pResource;
    PvgpuCmdOpenResource cmd;
    UINT32 hostHandle;

    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pResource = (PVGPU_UMD_RESOURCE*)hResource.pDrvPrivate;

    if (pResource == NULL)
    {
        PVGPU_TRACE("OpenResource: NULL resource handle");
        return;
    }

    /* Allocate a host-side resource handle */
    hostHandle = PvgpuAllocateResourceHandle(pDevice);

    /* Initialize local resource tracking */
    RtlZeroMemory(pResource, sizeof(PVGPU_UMD_RESOURCE));
    pResource->HostHandle = hostHandle;
    pResource->hRTResource = hRTResource;
    pResource->IsShared = TRUE;

    /* Fill in resource info from the open descriptor */
    if (pOpenResource->NumAllocations > 0)
    {
        pResource->Type = PVGPU_RESOURCE_TEXTURE_2D;  /* Shared resources are typically textures */
    }

    /* Build open resource command */
    RtlZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_OPEN_RESOURCE;
    cmd.header.command_size = sizeof(cmd);
    cmd.header.resource_id = hostHandle;

    /* The shared handle comes from the KMD allocation's private data */
    if (pOpenResource->pOpenAllocationInfo != NULL &&
        pOpenResource->NumAllocations > 0 &&
        pOpenResource->pOpenAllocationInfo[0].PrivateDriverDataSize >= sizeof(UINT32))
    {
        cmd.shared_handle = *((UINT32*)pOpenResource->pOpenAllocationInfo[0].pPrivateDriverData);
    }

    cmd.resource_type = pResource->Type;
    cmd.bind_flags = 0;

    /* Submit command */
    PvgpuWriteCommand(pDevice, PVGPU_CMD_OPEN_RESOURCE, &cmd, sizeof(cmd));

    PVGPU_TRACE("OpenResource: host handle %u, shared handle %u",
        hostHandle, cmd.shared_handle);
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
    cmd.header.command_type = PVGPU_CMD_CREATE_SHADER;
    cmd.header.command_size = sizeof(cmd);
    cmd.shader_id = pShader->HostHandle;
    cmd.shader_type = shaderType;
    cmd.bytecode_size = (UINT32)bytecodeSize;
    /* Allocate heap space and copy shader bytecode to shared memory */
    if (pDevice->SharedMemoryValid && pDevice->pHeap != NULL && bytecodeSize > 0)
    {
        UINT32 heapOffset = 0;
        HRESULT hr = PvgpuHeapAlloc(pDevice, (UINT32)bytecodeSize, 16, &heapOffset);
        if (SUCCEEDED(hr))
        {
            CopyMemory(pDevice->pHeap + heapOffset, pCode, bytecodeSize);
            cmd.bytecode_offset = heapOffset;
        }
        else
        {
            PVGPU_TRACE("Failed to allocate heap for shader bytecode, hr=0x%X", hr);
        }
    }
    
    /* Submit command */
    PvgpuWriteCommand(pDevice, PVGPU_CMD_CREATE_SHADER, &cmd, sizeof(cmd));
    
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

void APIENTRY PvgpuCreateHullShader(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST UINT* pCode,
    _In_ D3D10DDI_HSHADER hShader,
    _In_ D3D10DDI_HRTSHADER hRTShader,
    _In_ CONST D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES* pSignatures)
{
    UNREFERENCED_PARAMETER(hRTShader);
    UNREFERENCED_PARAMETER(pSignatures);
    
    PvgpuCreateShaderInternal(hDevice, pCode, hShader, PVGPU_SHADER_HULL);
}

void APIENTRY PvgpuCreateDomainShader(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST UINT* pCode,
    _In_ D3D10DDI_HSHADER hShader,
    _In_ D3D10DDI_HRTSHADER hRTShader,
    _In_ CONST D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES* pSignatures)
{
    UNREFERENCED_PARAMETER(hRTShader);
    UNREFERENCED_PARAMETER(pSignatures);
    
    PvgpuCreateShaderInternal(hDevice, pCode, hShader, PVGPU_SHADER_DOMAIN);
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
    
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_DESTROY_SHADER;
    cmd.header.command_size = sizeof(cmd);
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
    cmd.header.command_type = PVGPU_CMD_DRAW;
    cmd.header.command_size = sizeof(cmd);
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
    cmd.header.command_type = PVGPU_CMD_DRAW_INDEXED;
    cmd.header.command_size = sizeof(cmd);
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
    PvgpuCmdDrawInstanced cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_DRAW_INSTANCED;
    cmd.header.command_size = sizeof(cmd);
    cmd.vertex_count = VertexCountPerInstance;
    cmd.instance_count = InstanceCount;
    cmd.start_vertex = StartVertexLocation;
    cmd.start_instance = StartInstanceLocation;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_DRAW_INSTANCED, &cmd, sizeof(cmd));
    
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
    PvgpuCmdDrawIndexedInstanced cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_DRAW_INDEXED_INSTANCED;
    cmd.header.command_size = sizeof(cmd);
    cmd.index_count = IndexCountPerInstance;
    cmd.instance_count = InstanceCount;
    cmd.start_index = StartIndexLocation;
    cmd.base_vertex = BaseVertexLocation;
    cmd.start_instance = StartInstanceLocation;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_DRAW_INDEXED_INSTANCED, &cmd, sizeof(cmd));
    
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
    {
        PVGPU_UMD_RENDER_TARGET_VIEW* pRTV = (PVGPU_UMD_RENDER_TARGET_VIEW*)hRenderTargetView.pDrvPrivate;
        
        ZeroMemory(&cmd, sizeof(cmd));
        cmd.header.command_type = PVGPU_CMD_CLEAR_RENDER_TARGET;
        cmd.header.command_size = sizeof(cmd);
        cmd.rtv_id = (pRTV != NULL) ? pRTV->HostHandle : 0;
    }
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
    
    UNREFERENCED_PARAMETER(ClearFlags);
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    
    {
        PVGPU_UMD_DEPTH_STENCIL_VIEW* pDSV = (PVGPU_UMD_DEPTH_STENCIL_VIEW*)hDepthStencilView.pDrvPrivate;
        
        ZeroMemory(&cmd, sizeof(cmd));
        cmd.header.command_type = PVGPU_CMD_CLEAR_DEPTH_STENCIL;
        cmd.header.command_size = sizeof(cmd);
        cmd.dsv_id = (pDSV != NULL) ? pDSV->HostHandle : 0;
    }
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

void APIENTRY PvgpuHsSetShader(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HSHADER hShader)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_SHADER* pShader;
    PvgpuCmdSetShader cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pShader = (PVGPU_UMD_SHADER*)hShader.pDrvPrivate;
    
    /* Track in device state */
    pDevice->PipelineState.HullShader = pShader ? pShader->HostHandle : 0;
    
    /* Build command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_SET_SHADER;
    cmd.header.command_size = sizeof(cmd);
    cmd.stage = PVGPU_STAGE_HULL;
    cmd.shader_id = pDevice->PipelineState.HullShader;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_SET_SHADER, &cmd, sizeof(cmd));
}

void APIENTRY PvgpuDsSetShader(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HSHADER hShader)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_SHADER* pShader;
    PvgpuCmdSetShader cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pShader = (PVGPU_UMD_SHADER*)hShader.pDrvPrivate;
    
    /* Track in device state */
    pDevice->PipelineState.DomainShader = pShader ? pShader->HostHandle : 0;
    
    /* Build command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_SET_SHADER;
    cmd.header.command_size = sizeof(cmd);
    cmd.stage = PVGPU_STAGE_DOMAIN;
    cmd.shader_id = pDevice->PipelineState.DomainShader;
    
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
    UINT32 heapOffset = 0;
    SIZE_T dataSize;
    UINT width, height, depth;
    HRESULT hr;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pDst = (PVGPU_UMD_RESOURCE*)hDstResource.pDrvPrivate;
    
    if (pDst == NULL || pSysMemUP == NULL)
    {
        return;
    }
    
    /* Calculate dimensions */
    if (pDstBox != NULL)
    {
        width = pDstBox->right - pDstBox->left;
        height = pDstBox->bottom - pDstBox->top;
        depth = pDstBox->back - pDstBox->front;
    }
    else
    {
        width = pDst->Width;
        height = pDst->Height;
        depth = pDst->Depth > 0 ? pDst->Depth : 1;
    }
    
    /* Calculate data size */
    if (depth > 1)
    {
        dataSize = (SIZE_T)DepthPitch * depth;
    }
    else if (height > 1)
    {
        dataSize = (SIZE_T)RowPitch * height;
    }
    else
    {
        dataSize = (SIZE_T)RowPitch;
    }
    
    /* Try to allocate heap space and copy data */
    if (pDevice->SharedMemoryValid && pDevice->pHeap != NULL && dataSize > 0)
    {
        hr = PvgpuHeapAlloc(pDevice, (UINT32)dataSize, 16, &heapOffset);
        if (SUCCEEDED(hr))
        {
            /* Copy data to shared memory heap */
            UINT8* pDest = pDevice->pHeap + heapOffset;
            CopyMemory(pDest, pSysMemUP, dataSize);
            
            PVGPU_TRACE("UpdateSubresourceUP: Copied %zu bytes to heap offset %u",
                dataSize, heapOffset);
        }
        else
        {
            PVGPU_TRACE("UpdateSubresourceUP: Heap alloc failed, hr=0x%08X", hr);
            heapOffset = 0;
        }
    }
    
    /* Build command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_UPDATE_RESOURCE;
    cmd.header.command_size = sizeof(cmd);
    cmd.header.resource_id = pDst->HostHandle;
    cmd.subresource = DstSubresource;
    cmd.row_pitch = RowPitch;
    cmd.depth_pitch = DepthPitch;
    
    /* Set destination region */
    if (pDstBox != NULL)
    {
        cmd.dst_x = pDstBox->left;
        cmd.dst_y = pDstBox->top;
        cmd.dst_z = pDstBox->front;
        cmd.width = width;
        cmd.height = height;
        cmd.depth = depth;
    }
    else
    {
        cmd.dst_x = 0;
        cmd.dst_y = 0;
        cmd.dst_z = 0;
        cmd.width = width;
        cmd.height = height;
        cmd.depth = depth;
    }
    
    cmd.heap_offset = heapOffset;
    cmd.data_size = (UINT32)dataSize;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_UPDATE_RESOURCE, &cmd, sizeof(cmd));
    
    PVGPU_TRACE("UpdateSubresourceUP: resource %u subres %u size=%zu heap_offset=%u",
        pDst->HostHandle, DstSubresource, dataSize, heapOffset);
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
    UINT32 heapOffset = 0;
    SIZE_T mapSize;
    HRESULT hr;
    
    UNREFERENCED_PARAMETER(MapFlags);
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pResource = (PVGPU_UMD_RESOURCE*)hResource.pDrvPrivate;
    
    if (pResource == NULL || pMappedSubresource == NULL)
    {
        return;
    }
    
    /* Calculate map size - this is simplified, real impl would use format */
    if (pResource->Type == PVGPU_RESOURCE_TYPE_BUFFER)
    {
        mapSize = pResource->ByteWidth;
    }
    else
    {
        /* For textures, estimate based on dimensions (4 bytes per pixel) */
        mapSize = (SIZE_T)pResource->Width * pResource->Height * 4;
    }
    
    /* Default to failure */
    pMappedSubresource->pData = NULL;
    pMappedSubresource->RowPitch = 0;
    pMappedSubresource->DepthPitch = 0;
    
    /* Try to allocate heap space */
    if (!pDevice->SharedMemoryValid || pDevice->pHeap == NULL)
    {
        PVGPU_TRACE("ResourceMap: No shared memory available");
        return;
    }
    
    hr = PvgpuHeapAlloc(pDevice, (UINT32)mapSize, 16, &heapOffset);
    if (FAILED(hr))
    {
        PVGPU_TRACE("ResourceMap: Heap alloc failed for %zu bytes", mapSize);
        return;
    }
    
    /* Build map command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_MAP_RESOURCE;
    cmd.header.command_size = sizeof(cmd);
    cmd.header.resource_id = pResource->HostHandle;
    cmd.subresource = Subresource;
    cmd.map_type = MapType;
    cmd.heap_offset = heapOffset;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_MAP_RESOURCE, &cmd, sizeof(cmd));
    
    /* For read maps, flush commands and wait for host to copy data */
    if (MapType == D3D10_DDI_MAP_READ || MapType == D3D10_DDI_MAP_READWRITE)
    {
        /* Submit a fence and wait for it */
        UINT64 fenceValue = pDevice->NextFenceValue++;
        PvgpuCmdFence fenceCmd;
        
        ZeroMemory(&fenceCmd, sizeof(fenceCmd));
        fenceCmd.header.command_type = PVGPU_CMD_FENCE;
        fenceCmd.header.command_size = sizeof(fenceCmd);
        fenceCmd.fence_value = fenceValue;
        
        PvgpuWriteCommand(pDevice, PVGPU_CMD_FENCE, &fenceCmd, sizeof(fenceCmd));
        PvgpuFlushCommandBuffer(pDevice);
        
        /* Wait for fence completion */
        hr = PvgpuWaitFence(pDevice, fenceValue, 5000); /* 5 second timeout */
        if (FAILED(hr))
        {
            PVGPU_TRACE("ResourceMap: Fence wait failed");
            PvgpuHeapFree(pDevice, heapOffset, (UINT32)mapSize);
            return;
        }
    }
    
    /* Store mapping info */
    pResource->IsMapped = TRUE;
    pResource->MappedAddress = pDevice->pHeap + heapOffset;
    pResource->MappedSize = mapSize;
    
    /* Return mapped pointer */
    pMappedSubresource->pData = pDevice->pHeap + heapOffset;
    pMappedSubresource->RowPitch = pResource->Width * 4; /* Simplified - assume 4 bytes/pixel */
    pMappedSubresource->DepthPitch = pMappedSubresource->RowPitch * pResource->Height;
    
    PVGPU_TRACE("ResourceMap: resource %u subres %u -> heap offset %u size %zu",
        pResource->HostHandle, Subresource, heapOffset, mapSize);
}

void APIENTRY PvgpuResourceUnmap(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HRESOURCE hResource,
    _In_ UINT Subresource)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_RESOURCE* pResource;
    PvgpuCmdUnmapResource cmd;
    UINT32 heapOffset;
    UINT32 heapSize;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pResource = (PVGPU_UMD_RESOURCE*)hResource.pDrvPrivate;
    
    if (pResource == NULL || !pResource->IsMapped)
    {
        return;
    }
    
    /* Calculate heap offset from mapped address */
    if (pDevice->SharedMemoryValid && pResource->MappedAddress != NULL)
    {
        heapOffset = (UINT32)((UINT8*)pResource->MappedAddress - pDevice->pHeap);
        heapSize = (UINT32)pResource->MappedSize;
    }
    else
    {
        heapOffset = 0;
        heapSize = 0;
    }
    
    /* Build unmap command - host will copy data back to resource if needed */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_UNMAP_RESOURCE;
    cmd.header.command_size = sizeof(cmd);
    cmd.header.resource_id = pResource->HostHandle;
    cmd.subresource = Subresource;
    cmd.heap_offset = heapOffset;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_UNMAP_RESOURCE, &cmd, sizeof(cmd));
    
    /* Flush to ensure unmap is processed before we free heap */
    PvgpuFlushCommandBuffer(pDevice);
    
    /* Free heap allocation */
    if (heapSize > 0)
    {
        PvgpuHeapFree(pDevice, heapOffset, heapSize);
    }
    
    /* Mark resource as unmapped */
    pResource->IsMapped = FALSE;
    pResource->MappedAddress = NULL;
    pResource->MappedSize = 0;
    
    PVGPU_TRACE("ResourceUnmap: resource %u subres %u freed heap at %u",
        pResource->HostHandle, Subresource, heapOffset);
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
    PvgpuCmdFence fenceCmd;
    UINT64 fenceValue;
    UINT syncInterval = 1;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    
    if (pDevice == NULL)
    {
        return;
    }
    
    /* Extract sync interval from present data if available */
    if (pPresentData != NULL)
    {
        syncInterval = pPresentData->SyncInterval;
    }
    
    /* Async present: wait for the PREVIOUS frame's fence, not this one.
     * This gives the host an entire frame interval to process commands,
     * eliminating the 16.6ms stall that blocking on the current fence causes.
     * Classic "double-buffered fence" approach used by real GPU drivers. */
    if (pDevice->LastPresentFence > 0)
    {
        /* Only wait if vsync is enabled; tearing mode doesn't need sync */
        if (syncInterval > 0)
        {
            /* Fast path: check shared memory fence first to avoid KMD escape */
            if (pDevice->SharedMemoryValid &&
                pDevice->pControlRegion->host_fence_completed < pDevice->LastPresentFence)
            {
                PvgpuWaitFence(pDevice, pDevice->LastPresentFence, 100);
            }
        }
    }
    
    /* Allocate fence value for this present */
    fenceValue = pDevice->NextFenceValue++;
    
    /* Submit present command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_PRESENT;
    cmd.header.command_size = sizeof(cmd);
    cmd.backbuffer_id = 0; /* Default backbuffer - TODO: extract from pPresentData->hSurfaceToPresent */
    cmd.sync_interval = syncInterval;
    cmd.flags = 0;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_PRESENT, &cmd, sizeof(cmd));
    
    /* Submit fence command to know when present is done */
    ZeroMemory(&fenceCmd, sizeof(fenceCmd));
    fenceCmd.header.command_type = PVGPU_CMD_FENCE;
    fenceCmd.header.command_size = sizeof(fenceCmd);
    fenceCmd.fence_value = fenceValue;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_FENCE, &fenceCmd, sizeof(fenceCmd));
    
    /* Flush commands to ring buffer */
    PvgpuFlushCommandBuffer(pDevice);
    
    /* Track this frame's fence for next present's wait */
    pDevice->LastPresentFence = fenceValue;
    pDevice->LastFenceSubmitted = fenceValue;
    
    PVGPU_TRACE("Present: sync_interval=%u fence=%llu", syncInterval, (unsigned long long)fenceValue);
}

void APIENTRY PvgpuBlt(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ DXGI_DDI_ARG_BLT* pBltData)
{
    PVGPU_UMD_DEVICE* pDevice;
    PvgpuCmdCopyResource cmd;
    PVGPU_UMD_RESOURCE* pSrc;
    PVGPU_UMD_RESOURCE* pDst;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    
    if (pDevice == NULL || pBltData == NULL)
    {
        return;
    }
    
    pSrc = (PVGPU_UMD_RESOURCE*)pBltData->hSrcResource.pDrvPrivate;
    pDst = (PVGPU_UMD_RESOURCE*)pBltData->hDstResource.pDrvPrivate;
    
    if (pSrc == NULL || pDst == NULL)
    {
        return;
    }
    
    /* Build copy command for blt */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_COPY_RESOURCE;
    cmd.header.command_size = sizeof(cmd);
    cmd.src_resource_id = pSrc->HostHandle;
    cmd.dst_resource_id = pDst->HostHandle;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_COPY_RESOURCE, &cmd, sizeof(cmd));
    
    PVGPU_TRACE("Blt: src=%u dst=%u", pSrc->HostHandle, pDst->HostHandle);
}

HRESULT APIENTRY PvgpuResizeBuffers(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ DXGI_DDI_ARG_RESIZEBUFFERS* pResizeData)
{
    PVGPU_UMD_DEVICE* pDevice;
    PvgpuCmdResizeBuffers cmd;
    PvgpuCmdFence fenceCmd;
    UINT64 fenceValue;
    HRESULT hr;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    
    if (pDevice == NULL || pResizeData == NULL)
    {
        return E_INVALIDARG;
    }
    
    PVGPU_TRACE("ResizeBuffers: %ux%u format=%u buffers=%u",
        pResizeData->Width, pResizeData->Height,
        pResizeData->Format, pResizeData->BufferCount);
    
    /* Flush any pending commands before resize */
    PvgpuFlushCommandBuffer(pDevice);
    
    /* Allocate fence value for resize completion */
    fenceValue = pDevice->NextFenceValue++;
    
    /* Build resize buffers command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_RESIZE_BUFFERS;
    cmd.header.command_size = sizeof(cmd);
    cmd.swapchain_id = 0; /* Default swapchain */
    cmd.width = pResizeData->Width;
    cmd.height = pResizeData->Height;
    cmd.format = pResizeData->Format;
    cmd.buffer_count = pResizeData->BufferCount;
    cmd.flags = pResizeData->Flags;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_RESIZE_BUFFERS, &cmd, sizeof(cmd));
    
    /* Submit fence to know when resize is complete */
    ZeroMemory(&fenceCmd, sizeof(fenceCmd));
    fenceCmd.header.command_type = PVGPU_CMD_FENCE;
    fenceCmd.header.command_size = sizeof(fenceCmd);
    fenceCmd.fence_value = fenceValue;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_FENCE, &fenceCmd, sizeof(fenceCmd));
    
    /* Flush and wait for resize to complete */
    PvgpuFlushCommandBuffer(pDevice);
    
    hr = PvgpuWaitFence(pDevice, fenceValue, 5000); /* 5 second timeout */
    if (FAILED(hr))
    {
        PVGPU_TRACE("ResizeBuffers: Fence wait failed, hr=0x%08X", hr);
        /* Continue anyway - resize may have succeeded */
    }
    
    pDevice->LastFenceSubmitted = fenceValue;
    
    return S_OK;
}

/* ============================================================================
 * State Object Creation
 * ============================================================================ */

SIZE_T APIENTRY PvgpuCalcPrivateBlendStateSize(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10_DDI_BLEND_DESC* pBlendDesc)
{
    UNREFERENCED_PARAMETER(hDevice);
    UNREFERENCED_PARAMETER(pBlendDesc);
    return sizeof(PVGPU_UMD_BLEND_STATE);
}

void APIENTRY PvgpuCreateBlendState(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10_DDI_BLEND_DESC* pBlendDesc,
    _In_ D3D10DDI_HBLENDSTATE hBlendState,
    _In_ D3D10DDI_HRTBLENDSTATE hRTBlendState)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_BLEND_STATE* pState;
    PvgpuCmdCreateBlendState cmd;
    UINT i;
    
    UNREFERENCED_PARAMETER(hRTBlendState);
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pState = (PVGPU_UMD_BLEND_STATE*)hBlendState.pDrvPrivate;
    
    if (pState == NULL) return;
    
    /* Initialize state tracking */
    pState->HostHandle = PvgpuAllocateResourceHandle(pDevice);
    pState->AlphaToCoverageEnable = pBlendDesc->AlphaToCoverageEnable;
    pState->IndependentBlendEnable = pBlendDesc->IndependentBlendEnable;
    
    /* Build create command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_CREATE_BLEND_STATE;
    cmd.header.command_size = sizeof(cmd);
    cmd.state_id = pState->HostHandle;
    cmd.alpha_to_coverage = pBlendDesc->AlphaToCoverageEnable;
    cmd.independent_blend = pBlendDesc->IndependentBlendEnable;
    
    for (i = 0; i < 8; i++)
    {
        cmd.render_targets[i].blend_enable = pBlendDesc->RenderTarget[i].BlendEnable;
        cmd.render_targets[i].src_blend = pBlendDesc->RenderTarget[i].SrcBlend;
        cmd.render_targets[i].dest_blend = pBlendDesc->RenderTarget[i].DestBlend;
        cmd.render_targets[i].blend_op = pBlendDesc->RenderTarget[i].BlendOp;
        cmd.render_targets[i].src_blend_alpha = pBlendDesc->RenderTarget[i].SrcBlendAlpha;
        cmd.render_targets[i].dest_blend_alpha = pBlendDesc->RenderTarget[i].DestBlendAlpha;
        cmd.render_targets[i].blend_op_alpha = pBlendDesc->RenderTarget[i].BlendOpAlpha;
        cmd.render_targets[i].render_target_write_mask = pBlendDesc->RenderTarget[i].RenderTargetWriteMask;
    }
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_CREATE_BLEND_STATE, &cmd, sizeof(cmd));
    
    PVGPU_TRACE("Created blend state %u", pState->HostHandle);
}

void APIENTRY PvgpuDestroyBlendState(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HBLENDSTATE hBlendState)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_BLEND_STATE* pState;
    PvgpuCommandHeader cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pState = (PVGPU_UMD_BLEND_STATE*)hBlendState.pDrvPrivate;
    
    if (pState == NULL) return;
    
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.command_type = PVGPU_CMD_DESTROY_BLEND_STATE;
    cmd.command_size = sizeof(cmd);
    cmd.resource_id = pState->HostHandle;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_DESTROY_BLEND_STATE, &cmd, sizeof(cmd));
}

SIZE_T APIENTRY PvgpuCalcPrivateRasterizerStateSize(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10_DDI_RASTERIZER_DESC* pRasterizerDesc)
{
    UNREFERENCED_PARAMETER(hDevice);
    UNREFERENCED_PARAMETER(pRasterizerDesc);
    return sizeof(PVGPU_UMD_RASTERIZER_STATE);
}

void APIENTRY PvgpuCreateRasterizerState(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10_DDI_RASTERIZER_DESC* pRasterizerDesc,
    _In_ D3D10DDI_HRASTERIZERSTATE hRasterizerState,
    _In_ D3D10DDI_HRTRASTERIZERSTATE hRTRasterizerState)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_RASTERIZER_STATE* pState;
    PvgpuCmdCreateRasterizerState cmd;
    
    UNREFERENCED_PARAMETER(hRTRasterizerState);
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pState = (PVGPU_UMD_RASTERIZER_STATE*)hRasterizerState.pDrvPrivate;
    
    if (pState == NULL) return;
    
    /* Initialize state tracking */
    pState->HostHandle = PvgpuAllocateResourceHandle(pDevice);
    pState->FillMode = pRasterizerDesc->FillMode;
    pState->CullMode = pRasterizerDesc->CullMode;
    
    /* Build create command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_CREATE_RASTERIZER_STATE;
    cmd.header.command_size = sizeof(cmd);
    cmd.state_id = pState->HostHandle;
    cmd.fill_mode = pRasterizerDesc->FillMode;
    cmd.cull_mode = pRasterizerDesc->CullMode;
    cmd.front_counter_clockwise = pRasterizerDesc->FrontCounterClockwise;
    cmd.depth_bias = pRasterizerDesc->DepthBias;
    cmd.depth_bias_clamp = pRasterizerDesc->DepthBiasClamp;
    cmd.slope_scaled_depth_bias = pRasterizerDesc->SlopeScaledDepthBias;
    cmd.depth_clip_enable = pRasterizerDesc->DepthClipEnable;
    cmd.scissor_enable = pRasterizerDesc->ScissorEnable;
    cmd.multisample_enable = pRasterizerDesc->MultisampleEnable;
    cmd.antialiased_line_enable = pRasterizerDesc->AntialiasedLineEnable;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_CREATE_RASTERIZER_STATE, &cmd, sizeof(cmd));
    
    PVGPU_TRACE("Created rasterizer state %u", pState->HostHandle);
}

void APIENTRY PvgpuDestroyRasterizerState(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HRASTERIZERSTATE hRasterizerState)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_RASTERIZER_STATE* pState;
    PvgpuCommandHeader cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pState = (PVGPU_UMD_RASTERIZER_STATE*)hRasterizerState.pDrvPrivate;
    
    if (pState == NULL) return;
    
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.command_type = PVGPU_CMD_DESTROY_RASTERIZER_STATE;
    cmd.command_size = sizeof(cmd);
    cmd.resource_id = pState->HostHandle;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_DESTROY_RASTERIZER_STATE, &cmd, sizeof(cmd));
}

SIZE_T APIENTRY PvgpuCalcPrivateDepthStencilStateSize(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10_DDI_DEPTH_STENCIL_DESC* pDepthStencilDesc)
{
    UNREFERENCED_PARAMETER(hDevice);
    UNREFERENCED_PARAMETER(pDepthStencilDesc);
    return sizeof(PVGPU_UMD_DEPTH_STENCIL_STATE);
}

void APIENTRY PvgpuCreateDepthStencilState(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10_DDI_DEPTH_STENCIL_DESC* pDepthStencilDesc,
    _In_ D3D10DDI_HDEPTHSTENCILSTATE hDepthStencilState,
    _In_ D3D10DDI_HRTDEPTHSTENCILSTATE hRTDepthStencilState)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_DEPTH_STENCIL_STATE* pState;
    PvgpuCmdCreateDepthStencilState cmd;
    
    UNREFERENCED_PARAMETER(hRTDepthStencilState);
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pState = (PVGPU_UMD_DEPTH_STENCIL_STATE*)hDepthStencilState.pDrvPrivate;
    
    if (pState == NULL) return;
    
    /* Initialize state tracking */
    pState->HostHandle = PvgpuAllocateResourceHandle(pDevice);
    pState->DepthEnable = pDepthStencilDesc->DepthEnable;
    pState->StencilEnable = pDepthStencilDesc->StencilEnable;
    
    /* Build create command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_CREATE_DEPTH_STENCIL_STATE;
    cmd.header.command_size = sizeof(cmd);
    cmd.state_id = pState->HostHandle;
    cmd.depth_enable = pDepthStencilDesc->DepthEnable;
    cmd.depth_write_mask = pDepthStencilDesc->DepthWriteMask;
    cmd.depth_func = pDepthStencilDesc->DepthFunc;
    cmd.stencil_enable = pDepthStencilDesc->StencilEnable;
    cmd.stencil_read_mask = pDepthStencilDesc->StencilReadMask;
    cmd.stencil_write_mask = pDepthStencilDesc->StencilWriteMask;
    cmd.front_face.stencil_fail_op = pDepthStencilDesc->FrontFace.StencilFailOp;
    cmd.front_face.stencil_depth_fail_op = pDepthStencilDesc->FrontFace.StencilDepthFailOp;
    cmd.front_face.stencil_pass_op = pDepthStencilDesc->FrontFace.StencilPassOp;
    cmd.front_face.stencil_func = pDepthStencilDesc->FrontFace.StencilFunc;
    cmd.back_face.stencil_fail_op = pDepthStencilDesc->BackFace.StencilFailOp;
    cmd.back_face.stencil_depth_fail_op = pDepthStencilDesc->BackFace.StencilDepthFailOp;
    cmd.back_face.stencil_pass_op = pDepthStencilDesc->BackFace.StencilPassOp;
    cmd.back_face.stencil_func = pDepthStencilDesc->BackFace.StencilFunc;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_CREATE_DEPTH_STENCIL_STATE, &cmd, sizeof(cmd));
    
    PVGPU_TRACE("Created depth stencil state %u", pState->HostHandle);
}

void APIENTRY PvgpuDestroyDepthStencilState(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HDEPTHSTENCILSTATE hDepthStencilState)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_DEPTH_STENCIL_STATE* pState;
    PvgpuCommandHeader cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pState = (PVGPU_UMD_DEPTH_STENCIL_STATE*)hDepthStencilState.pDrvPrivate;
    
    if (pState == NULL) return;
    
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.command_type = PVGPU_CMD_DESTROY_DEPTH_STENCIL_STATE;
    cmd.command_size = sizeof(cmd);
    cmd.resource_id = pState->HostHandle;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_DESTROY_DEPTH_STENCIL_STATE, &cmd, sizeof(cmd));
}

SIZE_T APIENTRY PvgpuCalcPrivateSamplerSize(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10_DDI_SAMPLER_DESC* pSamplerDesc)
{
    UNREFERENCED_PARAMETER(hDevice);
    UNREFERENCED_PARAMETER(pSamplerDesc);
    return sizeof(PVGPU_UMD_SAMPLER);
}

void APIENTRY PvgpuCreateSampler(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10_DDI_SAMPLER_DESC* pSamplerDesc,
    _In_ D3D10DDI_HSAMPLER hSampler,
    _In_ D3D10DDI_HRTSAMPLER hRTSampler)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_SAMPLER* pState;
    PvgpuCmdCreateSampler cmd;
    
    UNREFERENCED_PARAMETER(hRTSampler);
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pState = (PVGPU_UMD_SAMPLER*)hSampler.pDrvPrivate;
    
    if (pState == NULL) return;
    
    /* Initialize state tracking */
    pState->HostHandle = PvgpuAllocateResourceHandle(pDevice);
    pState->Filter = pSamplerDesc->Filter;
    pState->AddressU = pSamplerDesc->AddressU;
    pState->AddressV = pSamplerDesc->AddressV;
    pState->AddressW = pSamplerDesc->AddressW;
    
    /* Build create command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_CREATE_SAMPLER;
    cmd.header.command_size = sizeof(cmd);
    cmd.sampler_id = pState->HostHandle;
    cmd.filter = pSamplerDesc->Filter;
    cmd.address_u = pSamplerDesc->AddressU;
    cmd.address_v = pSamplerDesc->AddressV;
    cmd.address_w = pSamplerDesc->AddressW;
    cmd.mip_lod_bias = pSamplerDesc->MipLODBias;
    cmd.max_anisotropy = pSamplerDesc->MaxAnisotropy;
    cmd.comparison_func = pSamplerDesc->ComparisonFunc;
    cmd.border_color[0] = pSamplerDesc->BorderColor[0];
    cmd.border_color[1] = pSamplerDesc->BorderColor[1];
    cmd.border_color[2] = pSamplerDesc->BorderColor[2];
    cmd.border_color[3] = pSamplerDesc->BorderColor[3];
    cmd.min_lod = pSamplerDesc->MinLOD;
    cmd.max_lod = pSamplerDesc->MaxLOD;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_CREATE_SAMPLER, &cmd, sizeof(cmd));
    
    PVGPU_TRACE("Created sampler %u", pState->HostHandle);
}

void APIENTRY PvgpuDestroySampler(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HSAMPLER hSampler)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_SAMPLER* pState;
    PvgpuCommandHeader cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pState = (PVGPU_UMD_SAMPLER*)hSampler.pDrvPrivate;
    
    if (pState == NULL) return;
    
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.command_type = PVGPU_CMD_DESTROY_SAMPLER;
    cmd.command_size = sizeof(cmd);
    cmd.resource_id = pState->HostHandle;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_DESTROY_SAMPLER, &cmd, sizeof(cmd));
}

SIZE_T APIENTRY PvgpuCalcPrivateElementLayoutSize(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10DDIARG_CREATEELEMENTLAYOUT* pCreateElementLayout)
{
    UNREFERENCED_PARAMETER(hDevice);
    UNREFERENCED_PARAMETER(pCreateElementLayout);
    return sizeof(PVGPU_UMD_INPUT_LAYOUT);
}

void APIENTRY PvgpuCreateElementLayout(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10DDIARG_CREATEELEMENTLAYOUT* pCreateElementLayout,
    _In_ D3D10DDI_HELEMENTLAYOUT hElementLayout,
    _In_ D3D10DDI_HRTELEMENTLAYOUT hRTElementLayout)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_INPUT_LAYOUT* pLayout;
    PvgpuCmdCreateInputLayout cmd;
    UINT i;
    
    UNREFERENCED_PARAMETER(hRTElementLayout);
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pLayout = (PVGPU_UMD_INPUT_LAYOUT*)hElementLayout.pDrvPrivate;
    
    if (pLayout == NULL) return;
    
    /* Initialize layout tracking */
    pLayout->HostHandle = PvgpuAllocateResourceHandle(pDevice);
    pLayout->NumElements = pCreateElementLayout->NumElements;
    
    /* Build create command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_CREATE_INPUT_LAYOUT;
    cmd.header.command_size = sizeof(cmd);
    cmd.layout_id = pLayout->HostHandle;
    cmd.num_elements = min(pCreateElementLayout->NumElements, 32);
    
    for (i = 0; i < cmd.num_elements; i++)
    {
        /* Copy semantic name string to shared memory */
        {
            PCSTR pSemanticName = pCreateElementLayout->pVertexElements[i].InputSemanticName;
            if (pSemanticName != NULL && pDevice->SharedMemoryValid && pDevice->pHeap != NULL)
            {
                SIZE_T nameLen = strlen(pSemanticName) + 1; /* Include null terminator */
                UINT32 nameOffset = 0;
                HRESULT hr = PvgpuHeapAlloc(pDevice, (UINT32)nameLen, 4, &nameOffset);
                if (SUCCEEDED(hr))
                {
                    CopyMemory(pDevice->pHeap + nameOffset, pSemanticName, nameLen);
                    cmd.elements[i].semantic_name_offset = nameOffset;
                }
                else
                {
                    cmd.elements[i].semantic_name_offset = 0;
                }
            }
            else
            {
                cmd.elements[i].semantic_name_offset = 0;
            }
        }
        cmd.elements[i].semantic_index = pCreateElementLayout->pVertexElements[i].SemanticIndex;
        cmd.elements[i].format = pCreateElementLayout->pVertexElements[i].Format;
        cmd.elements[i].input_slot = pCreateElementLayout->pVertexElements[i].InputSlot;
        cmd.elements[i].aligned_byte_offset = pCreateElementLayout->pVertexElements[i].AlignedByteOffset;
        cmd.elements[i].input_slot_class = pCreateElementLayout->pVertexElements[i].InputSlotClass;
        cmd.elements[i].instance_data_step_rate = pCreateElementLayout->pVertexElements[i].InstanceDataStepRate;
    }
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_CREATE_INPUT_LAYOUT, &cmd, sizeof(cmd));
    
    PVGPU_TRACE("Created input layout %u with %u elements", pLayout->HostHandle, cmd.num_elements);
}

void APIENTRY PvgpuDestroyElementLayout(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HELEMENTLAYOUT hElementLayout)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_INPUT_LAYOUT* pLayout;
    PvgpuCommandHeader cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pLayout = (PVGPU_UMD_INPUT_LAYOUT*)hElementLayout.pDrvPrivate;
    
    if (pLayout == NULL) return;
    
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.command_type = PVGPU_CMD_DESTROY_INPUT_LAYOUT;
    cmd.command_size = sizeof(cmd);
    cmd.resource_id = pLayout->HostHandle;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_DESTROY_INPUT_LAYOUT, &cmd, sizeof(cmd));
}

/* ============================================================================
 * View Creation
 * ============================================================================ */

SIZE_T APIENTRY PvgpuCalcPrivateRenderTargetViewSize(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10DDIARG_CREATERENDERTARGETVIEW* pCreateRenderTargetView)
{
    UNREFERENCED_PARAMETER(hDevice);
    UNREFERENCED_PARAMETER(pCreateRenderTargetView);
    return sizeof(PVGPU_UMD_RENDER_TARGET_VIEW);
}

void APIENTRY PvgpuCreateRenderTargetView(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10DDIARG_CREATERENDERTARGETVIEW* pCreateRenderTargetView,
    _In_ D3D10DDI_HRENDERTARGETVIEW hRenderTargetView,
    _In_ D3D10DDI_HRTRENDERTARGETVIEW hRTRenderTargetView)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_RENDER_TARGET_VIEW* pView;
    PVGPU_UMD_RESOURCE* pResource;
    PvgpuCmdCreateRenderTargetView cmd;
    
    UNREFERENCED_PARAMETER(hRTRenderTargetView);
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pView = (PVGPU_UMD_RENDER_TARGET_VIEW*)hRenderTargetView.pDrvPrivate;
    pResource = (PVGPU_UMD_RESOURCE*)pCreateRenderTargetView->hDrvResource.pDrvPrivate;
    
    if (pView == NULL) return;
    
    /* Initialize view tracking */
    pView->HostHandle = PvgpuAllocateResourceHandle(pDevice);
    pView->ResourceHandle = pResource ? pResource->HostHandle : 0;
    pView->Format = pCreateRenderTargetView->Format;
    
    /* Build create command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_CREATE_RENDER_TARGET_VIEW;
    cmd.header.command_size = sizeof(cmd);
    cmd.view_id = pView->HostHandle;
    cmd.resource_id = pView->ResourceHandle;
    cmd.format = pCreateRenderTargetView->Format;
    cmd.view_dimension = pCreateRenderTargetView->ResourceDimension;
    
    /* Copy dimension-specific data */
    switch (pCreateRenderTargetView->ResourceDimension)
    {
    case D3D10DDIRESOURCE_TEXTURE2D:
        cmd.u.texture2d.mip_slice = pCreateRenderTargetView->Tex2D.MipSlice;
        break;
    case D3D10DDIRESOURCE_TEXTURE2DARRAY:
        cmd.u.texture2d_array.mip_slice = pCreateRenderTargetView->Tex2DArray.MipSlice;
        cmd.u.texture2d_array.first_array_slice = pCreateRenderTargetView->Tex2DArray.FirstArraySlice;
        cmd.u.texture2d_array.array_size = pCreateRenderTargetView->Tex2DArray.ArraySize;
        break;
    default:
        break;
    }
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_CREATE_RENDER_TARGET_VIEW, &cmd, sizeof(cmd));
    
    PVGPU_TRACE("Created RTV %u for resource %u", pView->HostHandle, pView->ResourceHandle);
}

void APIENTRY PvgpuDestroyRenderTargetView(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HRENDERTARGETVIEW hRenderTargetView)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_RENDER_TARGET_VIEW* pView;
    PvgpuCommandHeader cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pView = (PVGPU_UMD_RENDER_TARGET_VIEW*)hRenderTargetView.pDrvPrivate;
    
    if (pView == NULL) return;
    
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.command_type = PVGPU_CMD_DESTROY_RENDER_TARGET_VIEW;
    cmd.command_size = sizeof(cmd);
    cmd.resource_id = pView->HostHandle;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_DESTROY_RENDER_TARGET_VIEW, &cmd, sizeof(cmd));
}

SIZE_T APIENTRY PvgpuCalcPrivateDepthStencilViewSize(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10DDIARG_CREATEDEPTHSTENCILVIEW* pCreateDepthStencilView)
{
    UNREFERENCED_PARAMETER(hDevice);
    UNREFERENCED_PARAMETER(pCreateDepthStencilView);
    return sizeof(PVGPU_UMD_DEPTH_STENCIL_VIEW);
}

void APIENTRY PvgpuCreateDepthStencilView(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10DDIARG_CREATEDEPTHSTENCILVIEW* pCreateDepthStencilView,
    _In_ D3D10DDI_HDEPTHSTENCILVIEW hDepthStencilView,
    _In_ D3D10DDI_HRTDEPTHSTENCILVIEW hRTDepthStencilView)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_DEPTH_STENCIL_VIEW* pView;
    PVGPU_UMD_RESOURCE* pResource;
    PvgpuCmdCreateDepthStencilView cmd;
    
    UNREFERENCED_PARAMETER(hRTDepthStencilView);
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pView = (PVGPU_UMD_DEPTH_STENCIL_VIEW*)hDepthStencilView.pDrvPrivate;
    pResource = (PVGPU_UMD_RESOURCE*)pCreateDepthStencilView->hDrvResource.pDrvPrivate;
    
    if (pView == NULL) return;
    
    /* Initialize view tracking */
    pView->HostHandle = PvgpuAllocateResourceHandle(pDevice);
    pView->ResourceHandle = pResource ? pResource->HostHandle : 0;
    pView->Format = pCreateDepthStencilView->Format;
    
    /* Build create command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_CREATE_DEPTH_STENCIL_VIEW;
    cmd.header.command_size = sizeof(cmd);
    cmd.view_id = pView->HostHandle;
    cmd.resource_id = pView->ResourceHandle;
    cmd.format = pCreateDepthStencilView->Format;
    cmd.view_dimension = pCreateDepthStencilView->ResourceDimension;
    cmd.flags = pCreateDepthStencilView->Flags;
    
    /* Copy dimension-specific data */
    switch (pCreateDepthStencilView->ResourceDimension)
    {
    case D3D10DDIRESOURCE_TEXTURE2D:
        cmd.u.texture2d.mip_slice = pCreateDepthStencilView->Tex2D.MipSlice;
        break;
    case D3D10DDIRESOURCE_TEXTURE2DARRAY:
        cmd.u.texture2d_array.mip_slice = pCreateDepthStencilView->Tex2DArray.MipSlice;
        cmd.u.texture2d_array.first_array_slice = pCreateDepthStencilView->Tex2DArray.FirstArraySlice;
        cmd.u.texture2d_array.array_size = pCreateDepthStencilView->Tex2DArray.ArraySize;
        break;
    default:
        break;
    }
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_CREATE_DEPTH_STENCIL_VIEW, &cmd, sizeof(cmd));
    
    PVGPU_TRACE("Created DSV %u for resource %u", pView->HostHandle, pView->ResourceHandle);
}

void APIENTRY PvgpuDestroyDepthStencilView(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HDEPTHSTENCILVIEW hDepthStencilView)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_DEPTH_STENCIL_VIEW* pView;
    PvgpuCommandHeader cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pView = (PVGPU_UMD_DEPTH_STENCIL_VIEW*)hDepthStencilView.pDrvPrivate;
    
    if (pView == NULL) return;
    
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.command_type = PVGPU_CMD_DESTROY_DEPTH_STENCIL_VIEW;
    cmd.command_size = sizeof(cmd);
    cmd.resource_id = pView->HostHandle;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_DESTROY_DEPTH_STENCIL_VIEW, &cmd, sizeof(cmd));
}

SIZE_T APIENTRY PvgpuCalcPrivateShaderResourceViewSize(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10DDIARG_CREATESHADERRESOURCEVIEW* pCreateShaderResourceView)
{
    UNREFERENCED_PARAMETER(hDevice);
    UNREFERENCED_PARAMETER(pCreateShaderResourceView);
    return sizeof(PVGPU_UMD_SHADER_RESOURCE_VIEW);
}

void APIENTRY PvgpuCreateShaderResourceView(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10DDIARG_CREATESHADERRESOURCEVIEW* pCreateShaderResourceView,
    _In_ D3D10DDI_HSHADERRESOURCEVIEW hShaderResourceView,
    _In_ D3D10DDI_HRTSHADERRESOURCEVIEW hRTShaderResourceView)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_SHADER_RESOURCE_VIEW* pView;
    PVGPU_UMD_RESOURCE* pResource;
    PvgpuCmdCreateShaderResourceView cmd;
    
    UNREFERENCED_PARAMETER(hRTShaderResourceView);
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pView = (PVGPU_UMD_SHADER_RESOURCE_VIEW*)hShaderResourceView.pDrvPrivate;
    pResource = (PVGPU_UMD_RESOURCE*)pCreateShaderResourceView->hDrvResource.pDrvPrivate;
    
    if (pView == NULL) return;
    
    /* Initialize view tracking */
    pView->HostHandle = PvgpuAllocateResourceHandle(pDevice);
    pView->ResourceHandle = pResource ? pResource->HostHandle : 0;
    pView->Format = pCreateShaderResourceView->Format;
    
    /* Build create command */
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_CREATE_SHADER_RESOURCE_VIEW;
    cmd.header.command_size = sizeof(cmd);
    cmd.view_id = pView->HostHandle;
    cmd.resource_id = pView->ResourceHandle;
    cmd.format = pCreateShaderResourceView->Format;
    cmd.view_dimension = pCreateShaderResourceView->ResourceDimension;
    
    /* Copy dimension-specific data */
    switch (pCreateShaderResourceView->ResourceDimension)
    {
    case D3D10DDIRESOURCE_TEXTURE2D:
        cmd.u.texture2d.most_detailed_mip = pCreateShaderResourceView->Tex2D.MostDetailedMip;
        cmd.u.texture2d.mip_levels = pCreateShaderResourceView->Tex2D.MipLevels;
        break;
    case D3D10DDIRESOURCE_TEXTURE2DARRAY:
        cmd.u.texture2d_array.most_detailed_mip = pCreateShaderResourceView->Tex2DArray.MostDetailedMip;
        cmd.u.texture2d_array.mip_levels = pCreateShaderResourceView->Tex2DArray.MipLevels;
        cmd.u.texture2d_array.first_array_slice = pCreateShaderResourceView->Tex2DArray.FirstArraySlice;
        cmd.u.texture2d_array.array_size = pCreateShaderResourceView->Tex2DArray.ArraySize;
        break;
    case D3D10DDIRESOURCE_BUFFER:
        cmd.u.buffer.first_element = pCreateShaderResourceView->Buffer.FirstElement;
        cmd.u.buffer.num_elements = pCreateShaderResourceView->Buffer.NumElements;
        break;
    default:
        break;
    }
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_CREATE_SHADER_RESOURCE_VIEW, &cmd, sizeof(cmd));
    
    PVGPU_TRACE("Created SRV %u for resource %u", pView->HostHandle, pView->ResourceHandle);
}

void APIENTRY PvgpuDestroyShaderResourceView(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HSHADERRESOURCEVIEW hShaderResourceView)
{
    PVGPU_UMD_DEVICE* pDevice;
    PVGPU_UMD_SHADER_RESOURCE_VIEW* pView;
    PvgpuCommandHeader cmd;
    
    pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    pView = (PVGPU_UMD_SHADER_RESOURCE_VIEW*)hShaderResourceView.pDrvPrivate;
    
    if (pView == NULL) return;
    
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.command_type = PVGPU_CMD_DESTROY_SHADER_RESOURCE_VIEW;
    cmd.command_size = sizeof(cmd);
    cmd.resource_id = pView->HostHandle;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_DESTROY_SHADER_RESOURCE_VIEW, &cmd, sizeof(cmd));
}

/* ============================================================================
 * Constant Buffers and Shader Resources Binding
 * ============================================================================ */

static void PvgpuSetConstantBuffersInternal(
    _In_ PVGPU_UMD_DEVICE* pDevice,
    _In_ PvgpuShaderStage stage,
    _In_ UINT StartBuffer,
    _In_ UINT NumBuffers,
    _In_reads_(NumBuffers) CONST D3D10DDI_HRESOURCE* phBuffers)
{
    PvgpuCmdSetConstantBuffer cmd;
    UINT i;
    
    for (i = 0; i < NumBuffers; i++)
    {
        PVGPU_UMD_RESOURCE* pBuffer = (PVGPU_UMD_RESOURCE*)phBuffers[i].pDrvPrivate;
        
        ZeroMemory(&cmd, sizeof(cmd));
        cmd.header.command_type = PVGPU_CMD_SET_CONSTANT_BUFFER;
        cmd.header.command_size = sizeof(cmd);
        cmd.stage = stage;
        cmd.slot = StartBuffer + i;
        cmd.buffer_id = pBuffer ? pBuffer->HostHandle : 0;
        cmd.offset = 0;
        cmd.size = pBuffer ? pBuffer->ByteWidth / 16 : 0; /* Size in 16-byte constants */
        
        PvgpuWriteCommand(pDevice, PVGPU_CMD_SET_CONSTANT_BUFFER, &cmd, sizeof(cmd));
    }
}

void APIENTRY PvgpuVsSetConstantBuffers(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT StartBuffer,
    _In_ UINT NumBuffers,
    _In_reads_(NumBuffers) CONST D3D10DDI_HRESOURCE* phBuffers)
{
    PVGPU_UMD_DEVICE* pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    PvgpuSetConstantBuffersInternal(pDevice, PVGPU_STAGE_VERTEX, StartBuffer, NumBuffers, phBuffers);
}

void APIENTRY PvgpuPsSetConstantBuffers(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT StartBuffer,
    _In_ UINT NumBuffers,
    _In_reads_(NumBuffers) CONST D3D10DDI_HRESOURCE* phBuffers)
{
    PVGPU_UMD_DEVICE* pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    PvgpuSetConstantBuffersInternal(pDevice, PVGPU_STAGE_PIXEL, StartBuffer, NumBuffers, phBuffers);
}

void APIENTRY PvgpuGsSetConstantBuffers(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT StartBuffer,
    _In_ UINT NumBuffers,
    _In_reads_(NumBuffers) CONST D3D10DDI_HRESOURCE* phBuffers)
{
    PVGPU_UMD_DEVICE* pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    PvgpuSetConstantBuffersInternal(pDevice, PVGPU_STAGE_GEOMETRY, StartBuffer, NumBuffers, phBuffers);
}

static void PvgpuSetShaderResourcesInternal(
    _In_ PVGPU_UMD_DEVICE* pDevice,
    _In_ PvgpuShaderStage stage,
    _In_ UINT Offset,
    _In_ UINT NumViews,
    _In_reads_(NumViews) CONST D3D10DDI_HSHADERRESOURCEVIEW* phShaderResourceViews)
{
    PvgpuCmdSetShaderResources cmd;
    UINT i;
    
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_SET_SHADER_RESOURCE;
    cmd.header.command_size = sizeof(cmd);
    cmd.stage = stage;
    cmd.start_slot = Offset;
    cmd.num_views = min(NumViews, 128);
    
    for (i = 0; i < cmd.num_views; i++)
    {
        PVGPU_UMD_SHADER_RESOURCE_VIEW* pView = 
            (PVGPU_UMD_SHADER_RESOURCE_VIEW*)phShaderResourceViews[i].pDrvPrivate;
        cmd.view_ids[i] = pView ? pView->HostHandle : 0;
    }
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_SET_SHADER_RESOURCE, &cmd, sizeof(cmd));
}

void APIENTRY PvgpuVsSetShaderResources(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT Offset,
    _In_ UINT NumViews,
    _In_reads_(NumViews) CONST D3D10DDI_HSHADERRESOURCEVIEW* phShaderResourceViews)
{
    PVGPU_UMD_DEVICE* pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    PvgpuSetShaderResourcesInternal(pDevice, PVGPU_STAGE_VERTEX, Offset, NumViews, phShaderResourceViews);
}

void APIENTRY PvgpuPsSetShaderResources(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT Offset,
    _In_ UINT NumViews,
    _In_reads_(NumViews) CONST D3D10DDI_HSHADERRESOURCEVIEW* phShaderResourceViews)
{
    PVGPU_UMD_DEVICE* pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    PvgpuSetShaderResourcesInternal(pDevice, PVGPU_STAGE_PIXEL, Offset, NumViews, phShaderResourceViews);
}

void APIENTRY PvgpuGsSetShaderResources(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT Offset,
    _In_ UINT NumViews,
    _In_reads_(NumViews) CONST D3D10DDI_HSHADERRESOURCEVIEW* phShaderResourceViews)
{
    PVGPU_UMD_DEVICE* pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    PvgpuSetShaderResourcesInternal(pDevice, PVGPU_STAGE_GEOMETRY, Offset, NumViews, phShaderResourceViews);
}

static void PvgpuSetSamplersInternal(
    _In_ PVGPU_UMD_DEVICE* pDevice,
    _In_ PvgpuShaderStage stage,
    _In_ UINT Offset,
    _In_ UINT NumSamplers,
    _In_reads_(NumSamplers) CONST D3D10DDI_HSAMPLER* phSamplers)
{
    PvgpuCmdSetSamplers cmd;
    UINT i;
    
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_SET_SAMPLER;
    cmd.header.command_size = sizeof(cmd);
    cmd.stage = stage;
    cmd.start_slot = Offset;
    cmd.num_samplers = min(NumSamplers, 16);
    
    for (i = 0; i < cmd.num_samplers; i++)
    {
        PVGPU_UMD_SAMPLER* pSampler = (PVGPU_UMD_SAMPLER*)phSamplers[i].pDrvPrivate;
        cmd.sampler_ids[i] = pSampler ? pSampler->HostHandle : 0;
    }
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_SET_SAMPLER, &cmd, sizeof(cmd));
}

void APIENTRY PvgpuVsSetSamplers(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT Offset,
    _In_ UINT NumSamplers,
    _In_reads_(NumSamplers) CONST D3D10DDI_HSAMPLER* phSamplers)
{
    PVGPU_UMD_DEVICE* pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    PvgpuSetSamplersInternal(pDevice, PVGPU_STAGE_VERTEX, Offset, NumSamplers, phSamplers);
}

void APIENTRY PvgpuPsSetSamplers(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT Offset,
    _In_ UINT NumSamplers,
    _In_reads_(NumSamplers) CONST D3D10DDI_HSAMPLER* phSamplers)
{
    PVGPU_UMD_DEVICE* pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    PvgpuSetSamplersInternal(pDevice, PVGPU_STAGE_PIXEL, Offset, NumSamplers, phSamplers);
}

void APIENTRY PvgpuGsSetSamplers(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT Offset,
    _In_ UINT NumSamplers,
    _In_reads_(NumSamplers) CONST D3D10DDI_HSAMPLER* phSamplers)
{
    PVGPU_UMD_DEVICE* pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    PvgpuSetSamplersInternal(pDevice, PVGPU_STAGE_GEOMETRY, Offset, NumSamplers, phSamplers);
}

/* ============================================================================
 * Hull/Domain Shader Stage Binding Functions
 * ============================================================================ */

void APIENTRY PvgpuHsSetConstantBuffers(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT StartBuffer,
    _In_ UINT NumBuffers,
    _In_reads_(NumBuffers) CONST D3D10DDI_HRESOURCE* phBuffers)
{
    PVGPU_UMD_DEVICE* pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    PvgpuSetConstantBuffersInternal(pDevice, PVGPU_STAGE_HULL, StartBuffer, NumBuffers, phBuffers);
}

void APIENTRY PvgpuDsSetConstantBuffers(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT StartBuffer,
    _In_ UINT NumBuffers,
    _In_reads_(NumBuffers) CONST D3D10DDI_HRESOURCE* phBuffers)
{
    PVGPU_UMD_DEVICE* pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    PvgpuSetConstantBuffersInternal(pDevice, PVGPU_STAGE_DOMAIN, StartBuffer, NumBuffers, phBuffers);
}

void APIENTRY PvgpuHsSetShaderResources(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT Offset,
    _In_ UINT NumViews,
    _In_reads_(NumViews) CONST D3D10DDI_HSHADERRESOURCEVIEW* phShaderResourceViews)
{
    PVGPU_UMD_DEVICE* pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    PvgpuSetShaderResourcesInternal(pDevice, PVGPU_STAGE_HULL, Offset, NumViews, phShaderResourceViews);
}

void APIENTRY PvgpuDsSetShaderResources(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT Offset,
    _In_ UINT NumViews,
    _In_reads_(NumViews) CONST D3D10DDI_HSHADERRESOURCEVIEW* phShaderResourceViews)
{
    PVGPU_UMD_DEVICE* pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    PvgpuSetShaderResourcesInternal(pDevice, PVGPU_STAGE_DOMAIN, Offset, NumViews, phShaderResourceViews);
}

void APIENTRY PvgpuHsSetSamplers(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT Offset,
    _In_ UINT NumSamplers,
    _In_reads_(NumSamplers) CONST D3D10DDI_HSAMPLER* phSamplers)
{
    PVGPU_UMD_DEVICE* pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    PvgpuSetSamplersInternal(pDevice, PVGPU_STAGE_HULL, Offset, NumSamplers, phSamplers);
}

void APIENTRY PvgpuDsSetSamplers(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT Offset,
    _In_ UINT NumSamplers,
    _In_reads_(NumSamplers) CONST D3D10DDI_HSAMPLER* phSamplers)
{
    PVGPU_UMD_DEVICE* pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    PvgpuSetSamplersInternal(pDevice, PVGPU_STAGE_DOMAIN, Offset, NumSamplers, phSamplers);
}

/* ============================================================================
 * Compute Shader DDI Functions
 * ============================================================================ */

/*
 * PvgpuCsSetShader - Set the compute shader
 */
void APIENTRY PvgpuCsSetShader(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HSHADER hShader)
{
    PVGPU_UMD_DEVICE* pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    PVGPU_UMD_SHADER* pShader = (PVGPU_UMD_SHADER*)hShader.pDrvPrivate;
    
    pDevice->PipelineState.ComputeShader = pShader ? pShader->HostHandle : 0;
    
    PvgpuCmdSetShader cmd;
    RtlZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_SET_SHADER;
    cmd.header.command_size = sizeof(cmd);
    cmd.stage = PVGPU_STAGE_COMPUTE;
    cmd.shader_id = pDevice->PipelineState.ComputeShader;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_SET_SHADER, &cmd, sizeof(cmd));
}

/*
 * PvgpuCsSetConstantBuffers - Set constant buffers for compute shader stage
 */
void APIENTRY PvgpuCsSetConstantBuffers(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT StartBuffer,
    _In_ UINT NumBuffers,
    _In_reads_(NumBuffers) CONST D3D10DDI_HRESOURCE* phBuffers)
{
    PVGPU_UMD_DEVICE* pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    PvgpuSetConstantBuffersInternal(pDevice, PVGPU_STAGE_COMPUTE, StartBuffer, NumBuffers, phBuffers);
}

/*
 * PvgpuCsSetShaderResources - Set shader resource views for compute shader stage
 */
void APIENTRY PvgpuCsSetShaderResources(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT Offset,
    _In_ UINT NumViews,
    _In_reads_(NumViews) CONST D3D10DDI_HSHADERRESOURCEVIEW* phShaderResourceViews)
{
    PVGPU_UMD_DEVICE* pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    PvgpuSetShaderResourcesInternal(pDevice, PVGPU_STAGE_COMPUTE, Offset, NumViews, phShaderResourceViews);
}

/*
 * PvgpuCsSetSamplers - Set samplers for compute shader stage
 */
void APIENTRY PvgpuCsSetSamplers(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT Offset,
    _In_ UINT NumSamplers,
    _In_reads_(NumSamplers) CONST D3D10DDI_HSAMPLER* phSamplers)
{
    PVGPU_UMD_DEVICE* pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    PvgpuSetSamplersInternal(pDevice, PVGPU_STAGE_COMPUTE, Offset, NumSamplers, phSamplers);
}

/*
 * PvgpuCsSetUnorderedAccessViews - Set UAVs for compute shader stage
 */
void APIENTRY PvgpuCsSetUnorderedAccessViews(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT Offset,
    _In_ UINT NumViews,
    _In_reads_(NumViews) CONST D3D11DDI_HUNORDEREDACCESSVIEW* phUnorderedAccessViews,
    _In_reads_(NumViews) CONST UINT* pUAVInitialCounts)
{
    PVGPU_UMD_DEVICE* pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    UINT i;
    
    UNREFERENCED_PARAMETER(pUAVInitialCounts);
    
    for (i = 0; i < NumViews && (Offset + i) < D3D11_1_UAV_SLOT_COUNT; i++) {
        PvgpuCmdSetShaderResource cmd;
        RtlZeroMemory(&cmd, sizeof(cmd));
        cmd.header.command_type = PVGPU_CMD_SET_SHADER_RESOURCE;
        cmd.header.command_size = sizeof(cmd);
        cmd.stage = PVGPU_STAGE_COMPUTE;
        cmd.slot = Offset + i;
        
        if (phUnorderedAccessViews[i].pDrvPrivate != NULL) {
            PVGPU_UMD_SHADER_RESOURCE_VIEW* pUAV = 
                (PVGPU_UMD_SHADER_RESOURCE_VIEW*)phUnorderedAccessViews[i].pDrvPrivate;
            cmd.view_id = pUAV->HostHandle;
        } else {
            cmd.view_id = 0;
        }
        
        PvgpuWriteCommand(pDevice, PVGPU_CMD_SET_SHADER_RESOURCE, &cmd, sizeof(cmd));
    }
}

/*
 * PvgpuDispatch - Dispatch compute shader execution
 */
void APIENTRY PvgpuDispatch(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT ThreadGroupCountX,
    _In_ UINT ThreadGroupCountY,
    _In_ UINT ThreadGroupCountZ)
{
    PVGPU_UMD_DEVICE* pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    
    PvgpuCmdDispatch cmd;
    RtlZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_DISPATCH;
    cmd.header.command_size = sizeof(cmd);
    cmd.thread_group_count_x = ThreadGroupCountX;
    cmd.thread_group_count_y = ThreadGroupCountY;
    cmd.thread_group_count_z = ThreadGroupCountZ;
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_DISPATCH, &cmd, sizeof(cmd));
}

/*
 * PvgpuDispatchIndirect - Dispatch compute shader with indirect args
 */
void APIENTRY PvgpuDispatchIndirect(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HRESOURCE hBufferForArgs,
    _In_ UINT AlignedByteOffsetForArgs)
{
    PVGPU_UMD_DEVICE* pDevice = (PVGPU_UMD_DEVICE*)hDevice.pDrvPrivate;
    
    PvgpuCmdDispatch cmd;
    RtlZeroMemory(&cmd, sizeof(cmd));
    cmd.header.command_type = PVGPU_CMD_DISPATCH;
    cmd.header.command_size = sizeof(cmd);
    
    /* For indirect dispatch, we encode the buffer resource and offset */
    if (hBufferForArgs.pDrvPrivate != NULL) {
        PVGPU_UMD_RESOURCE* pResource = (PVGPU_UMD_RESOURCE*)hBufferForArgs.pDrvPrivate;
        cmd.thread_group_count_x = pResource->HostHandle;
        cmd.thread_group_count_y = AlignedByteOffsetForArgs;
        cmd.thread_group_count_z = 0xFFFFFFFF; /* sentinel: indirect dispatch */
    }
    
    PvgpuWriteCommand(pDevice, PVGPU_CMD_DISPATCH, &cmd, sizeof(cmd));
}

/* ============================================================================
 * Command Buffer Helpers
 * ============================================================================ */

/*
 * PvgpuWriteCommand - Write a command to the staging buffer
 * 
 * Commands are first written to a local staging buffer, then copied
 * to the shared memory ring buffer on flush.
 */
BOOL PvgpuWriteCommand(
    _In_ PVGPU_UMD_DEVICE* pDevice,
    _In_ UINT32 CommandType,
    _In_ const void* pPayload,
    _In_ SIZE_T PayloadSize)
{
    SIZE_T alignedSize;

    if (!pDevice || !pDevice->pStagingBuffer || !pPayload || PayloadSize < sizeof(PvgpuCommandHeader))
        return FALSE;

    alignedSize = PVGPU_ALIGN16(PayloadSize);

    if (pDevice->StagingOffset + alignedSize > pDevice->StagingBufferSize)
        PvgpuFlushCommandBuffer(pDevice);

    CopyMemory(pDevice->pStagingBuffer + pDevice->StagingOffset, pPayload, PayloadSize);

    if (alignedSize > PayloadSize)
        ZeroMemory(pDevice->pStagingBuffer + pDevice->StagingOffset + PayloadSize, alignedSize - PayloadSize);

    pDevice->StagingOffset += alignedSize;
    pDevice->CommandsSubmitted++;

    return TRUE;
}

/*
 * PvgpuFlushCommandBuffer - Copy staged commands to ring buffer and notify host
 * 
 * This function:
 * 1. Waits if ring buffer is full (producer would catch up to consumer)
 * 2. Copies commands from staging buffer to ring buffer
 * 3. Updates producer pointer atomically
 * 4. Rings doorbell to wake up host backend
 */
void PvgpuFlushCommandBuffer(
    _In_ PVGPU_UMD_DEVICE* pDevice)
{
    SIZE_T spaceNeeded;
    SIZE_T spaceAvailable;
    UINT8* pWritePtr;
    SIZE_T writeOffset;
    SIZE_T firstChunkSize;
    SIZE_T secondChunkSize;
    
    if (pDevice == NULL || pDevice->StagingOffset == 0)
    {
        return;
    }
    
    /* If shared memory not available, just clear staging buffer */
    if (!pDevice->SharedMemoryValid || pDevice->pRingBuffer == NULL)
    {
        PVGPU_TRACE("FlushCommandBuffer: No shared memory, discarding %zu bytes",
            pDevice->StagingOffset);
        pDevice->StagingOffset = 0;
        return;
    }
    
    EnterCriticalSection(&pDevice->RingLock);
    
    spaceNeeded = pDevice->StagingOffset;
    
    /* Wait for space in ring buffer */
    for (;;)
    {
        UINT64 producer = pDevice->LocalProducerPtr;
        UINT64 consumer = pDevice->pControlRegion->consumer_ptr;
        UINT64 used = producer - consumer;
        
        spaceAvailable = pDevice->RingBufferSize - (SIZE_T)used;
        
        if (spaceAvailable >= spaceNeeded)
        {
            break;
        }
        
        /* Ring is full, need to wait for consumer to catch up */
        /* Hybrid spin-then-yield strategy for low latency */
        {
            static UINT spinCount = 0;
            spinCount++;
            
            LeaveCriticalSection(&pDevice->RingLock);
            
            if (spinCount < 100)
            {
                /* Spin: lowest latency for short waits */
                YieldProcessor();
            }
            else if (spinCount < 500)
            {
                /* Yield: give other threads a chance */
                SwitchToThread();
            }
            else
            {
                /* Sleep: prevent CPU waste on long waits */
                Sleep(1);
            }
            
            EnterCriticalSection(&pDevice->RingLock);
            
            /* Reset spin count when we acquire space */
            if (spinCount >= 500)
            {
                spinCount = 0;
            }
        }
    }
    
    /* Calculate write position (ring buffer is circular) */
    writeOffset = (SIZE_T)(pDevice->LocalProducerPtr % pDevice->RingBufferSize);
    pWritePtr = pDevice->pRingBuffer + writeOffset;
    
    /* Handle wrap-around */
    if (writeOffset + spaceNeeded <= pDevice->RingBufferSize)
    {
        /* Single copy - no wrap */
        CopyMemory(pWritePtr, pDevice->pStagingBuffer, spaceNeeded);
    }
    else
    {
        /* Two copies - wrap around */
        firstChunkSize = pDevice->RingBufferSize - writeOffset;
        secondChunkSize = spaceNeeded - firstChunkSize;
        
        CopyMemory(pWritePtr, pDevice->pStagingBuffer, firstChunkSize);
        CopyMemory(pDevice->pRingBuffer, 
                   pDevice->pStagingBuffer + firstChunkSize, 
                   secondChunkSize);
    }
    
    /* Memory barrier before updating producer pointer */
    MemoryBarrier();
    
    /* Update producer pointer atomically */
    pDevice->LocalProducerPtr += spaceNeeded;
    pDevice->pControlRegion->producer_ptr = pDevice->LocalProducerPtr;
    
    /* Another barrier to ensure write is visible */
    MemoryBarrier();
    
    LeaveCriticalSection(&pDevice->RingLock);
    
    /* Ring doorbell to notify host */
    PvgpuRingDoorbell(pDevice);
    
    /* Clear staging buffer */
    pDevice->StagingOffset = 0;
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

/* ============================================================================
 * KMD Escape Helpers
 * ============================================================================ */

/*
 * PvgpuEscape - Call KMD escape interface
 * 
 * This uses the D3D runtime callback to call DxgkDdiEscape in the KMD.
 */
HRESULT PvgpuEscape(
    _In_ PVGPU_UMD_DEVICE* pDevice,
    _Inout_ void* pEscapeData,
    _In_ SIZE_T EscapeDataSize)
{
    D3DDDICB_ESCAPE escapeData;
    HRESULT hr;
    
    if (pDevice == NULL || pDevice->pKTCallbacks == NULL)
    {
        return E_INVALIDARG;
    }
    
    if (pDevice->pKTCallbacks->pfnEscapeCb == NULL)
    {
        PVGPU_TRACE("PvgpuEscape: pfnEscapeCb is NULL");
        return E_NOTIMPL;
    }
    
    ZeroMemory(&escapeData, sizeof(escapeData));
    escapeData.hDevice = pDevice->hRTDevice.handle;
    escapeData.Flags.Value = 0;
    escapeData.pPrivateDriverData = pEscapeData;
    escapeData.PrivateDriverDataSize = (UINT)EscapeDataSize;
    escapeData.hContext = NULL;
    
    hr = pDevice->pKTCallbacks->pfnEscapeCb(pDevice->hRTDevice.handle, &escapeData);
    
    if (FAILED(hr))
    {
        PVGPU_TRACE("PvgpuEscape: pfnEscapeCb failed, hr=0x%08X", hr);
    }
    
    return hr;
}

/*
 * PvgpuInitSharedMemory - Initialize shared memory access
 * 
 * Calls PVGPU_ESCAPE_GET_SHMEM_INFO to get shared memory info from KMD.
 */
HRESULT PvgpuInitSharedMemory(
    _In_ PVGPU_UMD_DEVICE* pDevice)
{
    PvgpuEscapeGetShmemInfo info;
    HRESULT hr;
    
    if (pDevice == NULL)
    {
        return E_INVALIDARG;
    }
    
    ZeroMemory(&info, sizeof(info));
    info.header.escape_code = PVGPU_ESCAPE_GET_SHMEM_INFO;
    
    hr = PvgpuEscape(pDevice, &info, sizeof(info));
    if (FAILED(hr))
    {
        PVGPU_TRACE("PvgpuInitSharedMemory: Escape failed, hr=0x%08X", hr);
        return hr;
    }
    
    if (info.header.status != PVGPU_ERROR_SUCCESS)
    {
        PVGPU_TRACE("PvgpuInitSharedMemory: KMD returned error 0x%X", info.header.status);
        return E_FAIL;
    }
    
    /* Store shared memory info */
    pDevice->pSharedMemory = (void*)(ULONG_PTR)info.shmem_base;
    pDevice->SharedMemorySize = info.shmem_size;
    pDevice->pControlRegion = (PvgpuControlRegion*)pDevice->pSharedMemory;
    pDevice->pRingBuffer = (UINT8*)pDevice->pSharedMemory + info.ring_offset;
    pDevice->RingBufferSize = info.ring_size;
    pDevice->pHeap = (UINT8*)pDevice->pSharedMemory + info.heap_offset;
    pDevice->HeapSize = info.heap_size;
    pDevice->HeapOffset = info.heap_offset;
    pDevice->NegotiatedFeatures = info.features;
    
    /* Sync our local producer pointer with current value */
    pDevice->LocalProducerPtr = pDevice->pControlRegion->producer_ptr;
    
    pDevice->SharedMemoryValid = TRUE;
    pDevice->BackendConnected = TRUE;
    
    PVGPU_TRACE("SharedMemory init: base=%p size=%u ring=%u heap=%u features=0x%llX",
        pDevice->pSharedMemory,
        info.shmem_size,
        info.ring_size,
        info.heap_size,
        (unsigned long long)info.features);
    
    return S_OK;
}

/*
 * PvgpuHeapAlloc - Allocate from shared memory heap via KMD
 */
HRESULT PvgpuHeapAlloc(
    _In_ PVGPU_UMD_DEVICE* pDevice,
    _In_ UINT32 Size,
    _In_ UINT32 Alignment,
    _Out_ UINT32* pOffset)
{
    PvgpuEscapeAllocHeap alloc;
    HRESULT hr;
    
    if (pDevice == NULL || pOffset == NULL)
    {
        return E_INVALIDARG;
    }
    
    *pOffset = 0;
    
    ZeroMemory(&alloc, sizeof(alloc));
    alloc.header.escape_code = PVGPU_ESCAPE_ALLOC_HEAP;
    alloc.size = Size;
    alloc.alignment = Alignment > 0 ? Alignment : 16;
    
    hr = PvgpuEscape(pDevice, &alloc, sizeof(alloc));
    if (FAILED(hr))
    {
        return hr;
    }
    
    if (alloc.header.status != PVGPU_ERROR_SUCCESS)
    {
        PVGPU_TRACE("PvgpuHeapAlloc: KMD returned error 0x%X", alloc.header.status);
        return E_OUTOFMEMORY;
    }
    
    *pOffset = alloc.offset;
    return S_OK;
}

/*
 * PvgpuHeapFree - Free shared memory heap allocation via KMD
 */
HRESULT PvgpuHeapFree(
    _In_ PVGPU_UMD_DEVICE* pDevice,
    _In_ UINT32 Offset,
    _In_ UINT32 Size)
{
    PvgpuEscapeFreeHeap freeData;
    HRESULT hr;
    
    if (pDevice == NULL)
    {
        return E_INVALIDARG;
    }
    
    ZeroMemory(&freeData, sizeof(freeData));
    freeData.header.escape_code = PVGPU_ESCAPE_FREE_HEAP;
    freeData.offset = Offset;
    freeData.size = Size;
    
    hr = PvgpuEscape(pDevice, &freeData, sizeof(freeData));
    if (FAILED(hr))
    {
        return hr;
    }
    
    if (freeData.header.status != PVGPU_ERROR_SUCCESS)
    {
        PVGPU_TRACE("PvgpuHeapFree: KMD returned error 0x%X", freeData.header.status);
        return E_FAIL;
    }
    
    return S_OK;
}

/*
 * PvgpuRingDoorbell - Ring the doorbell to notify host of new commands
 */
HRESULT PvgpuRingDoorbell(
    _In_ PVGPU_UMD_DEVICE* pDevice)
{
    PvgpuEscapeHeader doorbell;
    HRESULT hr;
    
    if (pDevice == NULL)
    {
        return E_INVALIDARG;
    }
    
    ZeroMemory(&doorbell, sizeof(doorbell));
    doorbell.escape_code = PVGPU_ESCAPE_RING_DOORBELL;
    
    hr = PvgpuEscape(pDevice, &doorbell, sizeof(doorbell));
    /* Ignore errors - doorbell is best-effort notification */
    
    return hr;
}

/*
 * PvgpuWaitFence - Wait for a fence value to complete
 */
HRESULT PvgpuWaitFence(
    _In_ PVGPU_UMD_DEVICE* pDevice,
    _In_ UINT64 FenceValue,
    _In_ UINT32 TimeoutMs)
{
    PvgpuEscapeWaitFence wait;
    HRESULT hr;
    
    if (pDevice == NULL)
    {
        return E_INVALIDARG;
    }
    
    /* Check for backend disconnection before waiting */
    if (pDevice->SharedMemoryValid)
    {
        UINT32 status = pDevice->pControlRegion->status;
        if (status & PVGPU_STATUS_SHUTDOWN)
        {
            OutputDebugStringA("PVGPU: Backend has shut down\n");
            pDevice->BackendConnected = FALSE;
            return DXGI_ERROR_DEVICE_REMOVED;
        }
        if (status & PVGPU_STATUS_DEVICE_LOST)
        {
            OutputDebugStringA("PVGPU: Device lost\n");
            return DXGI_ERROR_DEVICE_REMOVED;
        }
    }
    
    /* Fast path: check if already completed */
    if (pDevice->SharedMemoryValid && 
        pDevice->pControlRegion->host_fence_completed >= FenceValue)
    {
        return S_OK;
    }
    
    ZeroMemory(&wait, sizeof(wait));
    wait.header.escape_code = PVGPU_ESCAPE_WAIT_FENCE;
    wait.fence_value = FenceValue;
    wait.timeout_ms = TimeoutMs;
    
    hr = PvgpuEscape(pDevice, &wait, sizeof(wait));
    if (FAILED(hr))
    {
        return hr;
    }
    
    if (wait.header.status == PVGPU_ERROR_TIMEOUT)
    {
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    else if (wait.header.status == PVGPU_ERROR_BACKEND_DISCONNECTED)
    {
        OutputDebugStringA("PVGPU: Backend disconnected during wait\n");
        pDevice->BackendConnected = FALSE;
        return DXGI_ERROR_DEVICE_REMOVED;
    }
    else if (wait.header.status == PVGPU_ERROR_DEVICE_LOST)
    {
        OutputDebugStringA("PVGPU: Device lost during wait\n");
        return DXGI_ERROR_DEVICE_REMOVED;
    }
    else if (wait.header.status != PVGPU_ERROR_SUCCESS)
    {
        return E_FAIL;
    }
    
    return S_OK;
}
